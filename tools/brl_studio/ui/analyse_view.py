"""
Analyse view — Phase 1.

Layout:
    ┌──────────────────────────────────────────────────────────────┐
    │ Lap-Picker (multi-select) │   Reference: [Lap 3 ▾]  Channels │
    ├───────────────────────────┴───────────────────┬──────────────┤
    │                                               │              │
    │                Chart (Speed / G)              │     Map      │
    │                                               │              │
    ├───────────────────────────────────────────────┴──────────────┤
    │                Sector bars (vs reference)                    │
    └──────────────────────────────────────────────────────────────┘

The chart's draggable cursor drives the map cursor by mapping distance
back to a (lat, lon) on the reference lap's track. When a Session is
pushed in via `set_session()`, channel arrays are derived once per lap
(see core/analysis.py) and cached in `self._cache`; toggling channels
just rebuilds the chart from the cache.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass
from pathlib import Path

from PyQt6.QtCore import QSettings, Qt, QThread, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSlider,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from core.analysis import LapChannels, delta_vs_reference, derive_channels
from core.export import export_lap_csv
from core.http_client import CamClient, Endpoint
from core.session import Lap, Session
from core.telemetry import Telemetry, load_telemetry_file, parse_telemetry
from core.tile_server import TileBounds
from core.video_dl import FileDownloadWorker, cache_dir
from ui.widgets.chart import LAP_COLORS, TelemetryChart, Trace
from ui.widgets.hud_overlay import HudOverlay
from ui.widgets.map import LeafletMap
from ui.widgets.sector_bars import SectorBars
from ui.widgets.tile_prefetch_dialog import TilePrefetchDialog
from ui.widgets.video_player import VideoPlayer, VideoStage


@dataclass
class _LapView:
    session: Session   # for the picker label + map title
    lap: Lap
    channels: LapChannels
    color: str
    selected: bool = True
    avi_path: Path | None = None         # filled by auto-download
    telemetry: Telemetry | None = None   # filled by auto-download


class _AutoDownloadWorker(QThread):
    """For each (session, lap), check the local cache + the cam HTTP API
    for a matching AVI/NDJSON pair. Found → download (if not yet cached)
    and report path back. Failure on a single lap is non-fatal — the
    worker just skips it and moves on."""

    lap_ready = pyqtSignal(int, str, object)  # cache_idx, avi_path,
                                              # telemetry-or-None
    progress = pyqtSignal(str)
    finished_all = pyqtSignal()

    def __init__(self, jobs: list[tuple[int, Session, Lap]],
                 cam_host: str, cam_port: int) -> None:
        super().__init__()
        self.jobs = jobs
        self.cam_host = cam_host
        self.cam_port = cam_port
        self._cancel = False

    def cancel(self) -> None:
        self._cancel = True

    def _candidate_ids(self, sess_id: str, lap_no: int) -> list[str]:
        """The cam writes its videos under different ID schemes depending
        on firmware version; probe a few common patterns."""
        return [
            f"{sess_id}/lap_{lap_no:02d}",
            f"{sess_id}/lap_{lap_no}",
            f"{sess_id}_lap_{lap_no:02d}",
            f"{sess_id}_lap_{lap_no}",
            sess_id,                        # legacy: one video per session
        ]

    def run(self) -> None:
        ep = Endpoint(host=self.cam_host, port=self.cam_port)
        client = CamClient(ep)
        try:
            videos = client.list_videos()
        except Exception:  # noqa: BLE001 — cam offline, that's fine
            videos = []
        avail = {str(v.get("id", "")) for v in videos if isinstance(v, dict)}

        for cache_idx, sess, lap in self.jobs:
            if self._cancel:
                break
            cands = self._candidate_ids(sess.id, lap.number)
            local_name = f"{sess.id}_lap_{lap.number:02d}.avi"
            local_path = cache_dir("videos") / local_name
            telem: Telemetry | None = None

            # 1. Already cached locally?
            if local_path.exists() and local_path.stat().st_size > 0:
                # Pair NDJSON if next to it
                ndj = local_path.with_suffix(".ndjson")
                if ndj.exists():
                    try:
                        telem = load_telemetry_file(ndj)
                    except (OSError, ValueError):
                        pass
                self.lap_ready.emit(cache_idx, str(local_path), telem)
                continue

            # 2. Find a matching ID in the cam's list and download.
            picked: str | None = None
            for c in cands:
                if c in avail:
                    picked = c
                    break
            if picked is None:
                continue   # no video for this lap, leave avi_path = None

            self.progress.emit(f"Lade Video: {picked}")
            try:
                with open(local_path, "wb") as f:
                    for chunk in client.stream_video(picked):
                        if self._cancel:
                            break
                        f.write(chunk)
                # Telemetry sidecar — try the session ID
                try:
                    text = client.get_telemetry_ndjson(sess.id)
                    ndj = local_path.with_suffix(".ndjson")
                    ndj.write_text(text, encoding="utf-8")
                    telem = parse_telemetry(text)
                except Exception:  # noqa: BLE001
                    pass
            except Exception:  # noqa: BLE001
                # Partial download — clean up so next attempt restarts
                try:
                    local_path.unlink(missing_ok=True)
                except OSError:
                    pass
                continue

            if local_path.exists() and local_path.stat().st_size > 0:
                self.lap_ready.emit(cache_idx, str(local_path), telem)

        self.finished_all.emit()


def _fmt_ms(ms: int) -> str:
    s = max(0, ms) / 1000.0
    m, s = divmod(s, 60.0)
    return f"{int(m)}:{s:06.3f}"


class AnalyseView(QWidget):
    def __init__(self, main_window) -> None:  # noqa: ANN001
        super().__init__()
        self._mw = main_window
        self._cache: list[_LapView] = []
        self._ref_idx: int = 0     # index into self._cache
        self._delta_cache: dict[int, list[float]] = {}
        self._auto_dl: _AutoDownloadWorker | None = None
        self._settings = QSettings()

        # ── Top bar — lap picker + reference picker + channels ─────────
        self.lap_list = QListWidget()
        self.lap_list.setMaximumWidth(220)
        self.lap_list.setStyleSheet(
            "QListWidget { background: #1e1e22; border: 0; }"
            "QListWidget::item { padding: 6px 8px; }"
        )
        self.lap_list.itemChanged.connect(self._on_lap_toggled)

        self.ref_combo = QComboBox()
        self.ref_combo.currentIndexChanged.connect(self._on_ref_changed)

        self.cb_speed   = QCheckBox("Speed")
        self.cb_g_lat   = QCheckBox("G-lat")
        self.cb_g_long  = QCheckBox("G-long")
        self.cb_delta   = QCheckBox("Delta")
        for cb in (self.cb_speed, self.cb_g_lat, self.cb_g_long, self.cb_delta):
            cb.setChecked(True)
            cb.toggled.connect(self._rebuild_chart)

        self.btn_export_csv = QPushButton("📤 CSV exportieren…")
        self.btn_export_csv.clicked.connect(self._export_csv)
        self.btn_cache_map = QPushButton("🗺 Karte cachen…")
        self.btn_cache_map.setToolTip(
            "Lädt OSM-Tiles dieser Strecke vorab in den Offline-Cache. "
            "Danach funktioniert die Karte auch ohne Internet (z. B. wenn "
            "der PC im Laptimer-WiFi hängt)."
        )
        self.btn_cache_map.clicked.connect(self._cache_map)
        self.cb_video_visible = QCheckBox("🎬 Video anzeigen")
        self.cb_video_visible.setChecked(False)
        self.cb_video_visible.toggled.connect(self._on_video_visibility_toggled)

        side = QVBoxLayout()
        side.addWidget(QLabel("Laps"))
        side.addWidget(self.lap_list, stretch=1)
        side.addSpacing(8)
        side.addWidget(QLabel("Referenz"))
        side.addWidget(self.ref_combo)
        side.addSpacing(12)
        side.addWidget(QLabel("Kanäle"))
        side.addWidget(self.cb_speed)
        side.addWidget(self.cb_g_lat)
        side.addWidget(self.cb_g_long)
        side.addWidget(self.cb_delta)
        side.addSpacing(12)
        side.addWidget(self.cb_video_visible)
        side.addWidget(self.btn_export_csv)
        side.addWidget(self.btn_cache_map)

        side_w = QWidget()
        side_w.setLayout(side)
        side_w.setMaximumWidth(240)

        # ── Video players A + B (auto-fed from the lap cache) ──────────
        # Player A is always present. Player B + its transport row only
        # show up when the user enables Dual-Modus.
        self.player_a = VideoPlayer()
        self.hud_a    = HudOverlay()
        self.stage_a  = VideoStage(self.player_a, self.hud_a)
        self.player_b = VideoPlayer()
        self.hud_b    = HudOverlay()
        self.stage_b  = VideoStage(self.player_b, self.hud_b)
        self.player_a.position_changed.connect(self._on_player_a_pos)
        self.player_b.position_changed.connect(self._on_player_b_pos)
        self.player_a.duration_changed.connect(self._on_player_a_dur)
        self.player_b.duration_changed.connect(self._on_player_b_dur)
        self._dual_busy = False

        self.video_split = QSplitter(Qt.Orientation.Horizontal)
        self.video_split.addWidget(self.stage_a)
        self.video_split.addWidget(self.stage_b)
        self.video_split.setSizes([1, 0])  # B hidden until dual is on

        # Transport row + dual/sync controls
        self.btn_play_a = QPushButton("▶"); self.btn_play_a.setFixedWidth(40)
        self.btn_play_a.clicked.connect(self._toggle_play_a)
        self.scrub_a = QSlider(Qt.Orientation.Horizontal); self.scrub_a.setRange(0, 0)
        self.scrub_a.sliderMoved.connect(self._on_scrub_a)
        self.lbl_time_a = QLabel("0:00.000"); self.lbl_time_a.setMinimumWidth(80)
        self.lbl_time_a.setStyleSheet("font-family: monospace;")

        self.btn_play_b = QPushButton("▶"); self.btn_play_b.setFixedWidth(40)
        self.btn_play_b.clicked.connect(self._toggle_play_b)
        self.scrub_b = QSlider(Qt.Orientation.Horizontal); self.scrub_b.setRange(0, 0)
        self.scrub_b.sliderMoved.connect(self._on_scrub_b)
        self.lbl_time_b = QLabel("0:00.000"); self.lbl_time_b.setMinimumWidth(80)
        self.lbl_time_b.setStyleSheet("font-family: monospace;")

        self.cb_dual = QCheckBox("Dual-Video")
        self.cb_dual.toggled.connect(self._set_dual)
        self.cb_sync = QCheckBox("Sync")
        self.cb_sync.setChecked(True)

        # Combo to pick which downloaded lap belongs to which player
        self.player_a_pick = QComboBox()
        self.player_b_pick = QComboBox()
        self.player_a_pick.currentIndexChanged.connect(
            lambda _: self._load_into_player(self.player_a_pick, self.player_a, self.hud_a)
        )
        self.player_b_pick.currentIndexChanged.connect(
            lambda _: self._load_into_player(self.player_b_pick, self.player_b, self.hud_b)
        )

        transport_a = QHBoxLayout()
        transport_a.addWidget(QLabel("A"))
        transport_a.addWidget(self.player_a_pick, stretch=2)
        transport_a.addWidget(self.btn_play_a)
        transport_a.addWidget(self.scrub_a, stretch=3)
        transport_a.addWidget(self.lbl_time_a)
        transport_a.addSpacing(12)
        transport_a.addWidget(self.cb_dual)
        transport_a.addWidget(self.cb_sync)

        self._transport_b_row = QHBoxLayout()
        self._transport_b_row.addWidget(QLabel("B"))
        self._transport_b_row.addWidget(self.player_b_pick, stretch=2)
        self._transport_b_row.addWidget(self.btn_play_b)
        self._transport_b_row.addWidget(self.scrub_b, stretch=3)
        self._transport_b_row.addWidget(self.lbl_time_b)

        # ── Chart + map (top half), video (lower half) ─────────────────
        self.chart = TelemetryChart()
        self.chart.cursor_moved.connect(self._on_cursor_moved)
        self.map = LeafletMap()

        chart_map = QSplitter(Qt.Orientation.Horizontal)
        chart_map.addWidget(self.chart)
        chart_map.addWidget(self.map)
        chart_map.setStretchFactor(0, 3)
        chart_map.setStretchFactor(1, 2)

        # Wrap the entire video area (stages + both transport rows) in a
        # single container so the "🎬 Video anzeigen"-checkbox can flip
        # the whole region with one setVisible() call.
        self._video_region = QWidget()
        video_region_layout = QVBoxLayout(self._video_region)
        video_region_layout.setContentsMargins(0, 0, 0, 0)
        video_region_layout.setSpacing(4)
        video_region_layout.addWidget(self.video_split, stretch=1)
        video_region_layout.addLayout(transport_a)
        video_region_layout.addLayout(self._transport_b_row)

        center = QSplitter(Qt.Orientation.Vertical)
        center.addWidget(chart_map)
        center.addWidget(self._video_region)
        center.setStretchFactor(0, 3)
        center.setStretchFactor(1, 2)

        # ── Bottom — sector bars ────────────────────────────────────────
        self.sector_bars = SectorBars()

        right = QVBoxLayout()
        right.setContentsMargins(0, 0, 0, 0)
        right.setSpacing(6)
        right.addWidget(center, stretch=1)
        right.addWidget(self.sector_bars)

        right_w = QWidget()
        right_w.setLayout(right)

        root = QHBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(8)
        root.addWidget(side_w)
        root.addWidget(right_w, stretch=1)

        # Hide Player B's transport row until Dual is enabled
        self._set_dual(False)
        # Default: video region hidden until at least one lap has a video.
        self._video_region.setVisible(False)

        # On Windows VLC paints into the player's native HWND and that
        # repaints clobber the Z-order of the sibling HUD widgets. A
        # cheap 500 ms timer just calls raise_() on the visible HUDs
        # to keep them on top — same pattern as the old VideoView.
        self._raise_timer = QTimer(self)
        self._raise_timer.setInterval(500)
        self._raise_timer.timeout.connect(self._raise_huds)
        self._raise_timer.start()

        # Arrow-key scrub on the chart cursor — distance-based steps so
        # the increment stays roughly the same regardless of how zoomed
        # the chart is. Holding Shift makes a coarse jump (1% of the
        # reference lap), holding nothing is the fine step (5 m).
        QShortcut(QKeySequence(Qt.Key.Key_Left),  self,
                  activated=lambda: self._nudge_cursor(-5.0))
        QShortcut(QKeySequence(Qt.Key.Key_Right), self,
                  activated=lambda: self._nudge_cursor(+5.0))
        QShortcut(QKeySequence("Shift+Left"),  self,
                  activated=lambda: self._nudge_cursor_pct(-0.01))
        QShortcut(QKeySequence("Shift+Right"), self,
                  activated=lambda: self._nudge_cursor_pct(+0.01))

        self._show_empty_state()

    def _raise_huds(self) -> None:
        self.hud_a.raise_()
        if self.cb_dual.isChecked():
            self.hud_b.raise_()

    # ── arrow-key scrubbing ────────────────────────────────────────────

    def _current_cursor_distance(self) -> float | None:
        """Read the chart's current cursor x-value (meters)."""
        if not self.chart._cursors:  # private but stable accessor
            return None
        try:
            return float(self.chart._cursors[0].value())
        except Exception:  # noqa: BLE001
            return None

    def _ref_total_distance(self) -> float:
        if not self._cache or not (0 <= self._ref_idx < len(self._cache)):
            return 0.0
        d = self._cache[self._ref_idx].channels.distance_m
        return float(d[-1]) if d else 0.0

    def _nudge_cursor(self, delta_m: float) -> None:
        """Move the chart cursor by `delta_m` along the X axis."""
        cur = self._current_cursor_distance()
        if cur is None:
            return
        total = self._ref_total_distance()
        nxt = cur + delta_m
        if total > 0:
            nxt = max(0.0, min(total, nxt))
        self.chart.set_cursor(nxt)
        # set_cursor does not fire cursor_moved (it's the
        # set-from-Python path) — drive map + video manually.
        self._on_cursor_moved(nxt)

    def _nudge_cursor_pct(self, pct: float) -> None:
        total = self._ref_total_distance()
        if total <= 0:
            return
        self._nudge_cursor(total * pct)

    # ── public API ──────────────────────────────────────────────────────

    def set_laps(self, pairs: list[tuple[Session, Lap]]) -> None:
        """Called by MainWindow with a cross-session lap selection.

        Cross-session is the normal mode now; if the basket has only one
        track all comparisons line up. If track names differ we still
        chart them (driver might be comparing two layouts), but the map
        background will be whatever the first lap's bbox is — the chart
        cursor → map cursor mapping then only makes sense for that track.
        """
        # Build the cache directly from the basket — preserves the user's
        # preferred order and which laps to include.
        self._cache.clear()
        self._delta_cache.clear()
        # Default reference: fastest lap in the set.
        valid = [(i, s, l) for i, (s, l) in enumerate(pairs)
                 if l.points and l.total_ms > 0]
        if not valid:
            self._show_empty_state()
            self._mw.set_status("Keine analysierbaren Laps")
            return
        ref_pos = min(valid, key=lambda x: x[2].total_ms)[0]
        self._ref_idx = ref_pos
        non_ref = 1
        for i, (s, lap) in enumerate(pairs):
            if i == ref_pos:
                color = LAP_COLORS[0]
            else:
                color = LAP_COLORS[((non_ref - 1) % (len(LAP_COLORS) - 1)) + 1]
                non_ref += 1
            self._cache.append(_LapView(
                session=s, lap=lap,
                channels=derive_channels(lap),
                color=color,
            ))
        self._rebuild_lap_picker()
        self._rebuild_ref_combo()
        self._rebuild_chart()
        self._rebuild_map_traces()
        self._rebuild_sector_bars()
        self._rebuild_player_picks()
        # New lap set: hide the video region until something downloads.
        # If the checkbox-state survives across calls (the user *wants*
        # video visible), respect that — _on_lap_video_ready re-checks
        # it once the first AVI arrives.
        self.cb_video_visible.setChecked(False)
        # Fit map to bounds of the active lap set
        self.map.fit_bounds()
        tracks = sorted({lv.session.track for lv in self._cache})
        self._mw.set_status(
            f"Analyse: {len(self._cache)} Lap(s) aus "
            f"{len(tracks)} Strecke(n) — {', '.join(tracks)}"
        )
        # Kick off background auto-download of any matching cam videos
        # for the chosen laps. Found videos populate _cache[i].avi_path /
        # .telemetry; the player-pick combos refresh as each lap arrives.
        self._kick_auto_download()

    # ── building blocks ────────────────────────────────────────────────

    def _show_empty_state(self) -> None:
        self.chart.clear()
        self.sector_bars.set_sectors([])

    def _picker_label(self, lv: _LapView, ref: bool = False) -> str:
        """Compact label that still tells you which session a lap is from."""
        sname = lv.session.name or lv.session.id
        # Trim long session names so the picker stays readable
        if len(sname) > 22:
            sname = sname[:21] + "…"
        return (f"{sname} · L{lv.lap.number} · "
                f"{lv.lap.lap_time_str()}{' (ref)' if ref else ''}")

    def _rebuild_lap_picker(self) -> None:
        self.lap_list.blockSignals(True)
        self.lap_list.clear()
        for i, lv in enumerate(self._cache):
            item = QListWidgetItem(self._picker_label(lv, ref=(i == self._ref_idx)))
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(
                Qt.CheckState.Checked if lv.selected else Qt.CheckState.Unchecked
            )
            item.setForeground(QColor(lv.color))
            item.setToolTip(
                f"Strecke: {lv.session.track}\n"
                f"Session: {lv.session.name or lv.session.id}\n"
                f"Lap {lv.lap.number} — {lv.lap.lap_time_str()}"
            )
            self.lap_list.addItem(item)
        self.lap_list.blockSignals(False)

    def _rebuild_ref_combo(self) -> None:
        self.ref_combo.blockSignals(True)
        self.ref_combo.clear()
        for lv in self._cache:
            self.ref_combo.addItem(self._picker_label(lv))
        self.ref_combo.setCurrentIndex(self._ref_idx)
        self.ref_combo.blockSignals(False)

    def _on_lap_toggled(self, item: QListWidgetItem) -> None:
        idx = self.lap_list.row(item)
        if 0 <= idx < len(self._cache):
            self._cache[idx].selected = item.checkState() == Qt.CheckState.Checked
            self._rebuild_chart()
            self._rebuild_map_traces()

    def _on_ref_changed(self, idx: int) -> None:
        if not (0 <= idx < len(self._cache)):
            return
        # Re-color: ref always gets LAP_COLORS[0], others rotate.
        self._ref_idx = idx
        self._delta_cache.clear()
        non_ref_color = 1
        for i, lv in enumerate(self._cache):
            if i == idx:
                lv.color = LAP_COLORS[0]
            else:
                lv.color = LAP_COLORS[
                    ((non_ref_color - 1) % (len(LAP_COLORS) - 1)) + 1
                ]
                non_ref_color += 1
        self._rebuild_lap_picker()
        self._rebuild_chart()
        self._rebuild_map_traces()
        self._rebuild_sector_bars()

    def _selected(self) -> list[tuple[int, _LapView]]:
        return [(i, lv) for i, lv in enumerate(self._cache) if lv.selected]

    def _delta_for(self, lap_idx: int) -> list[float]:
        if lap_idx == self._ref_idx:
            return [0.0] * len(self._cache[lap_idx].lap.points)
        cached = self._delta_cache.get(lap_idx)
        if cached is None:
            cached = delta_vs_reference(
                self._cache[lap_idx].lap, self._cache[self._ref_idx].lap
            )
            self._delta_cache[lap_idx] = cached
        return cached

    def _rebuild_chart(self) -> None:
        if not self._cache:
            self.chart.clear()
            return

        sel = self._selected()
        if not sel:
            self.chart.clear()
            return

        channels: list[tuple[str, str, list[Trace]]] = []

        def label(lv: _LapView, idx: int) -> str:
            return f"Lap {lv.lap.number}" + (" (ref)" if idx == self._ref_idx else "")

        if self.cb_speed.isChecked():
            traces = [
                Trace(label(lv, i), lv.color,
                      lv.channels.distance_m, lv.channels.speed_kmh)
                for i, lv in sel
            ]
            channels.append(("Speed", "km/h", traces))

        if self.cb_g_lat.isChecked():
            traces = [
                Trace(label(lv, i), lv.color,
                      lv.channels.distance_m, lv.channels.g_lat)
                for i, lv in sel
            ]
            channels.append(("G-lat", "g", traces))

        if self.cb_g_long.isChecked():
            traces = [
                Trace(label(lv, i), lv.color,
                      lv.channels.distance_m, lv.channels.g_long)
                for i, lv in sel
            ]
            channels.append(("G-long", "g", traces))

        if self.cb_delta.isChecked():
            traces = []
            for i, lv in sel:
                if i == self._ref_idx:
                    continue
                traces.append(
                    Trace(label(lv, i), lv.color,
                          lv.channels.distance_m, self._delta_for(i))
                )
            if traces:
                channels.append(("Delta", "s", traces))

        self.chart.set_channels(channels)

    def _rebuild_map_traces(self) -> None:
        traces = []
        for i, lv in self._selected():
            traces.append({
                "color": lv.color,
                "points": [[p.lat, p.lon] for p in lv.lap.points],
            })
        self.map.set_traces(traces)

    def _rebuild_sector_bars(self) -> None:
        if not self._cache or not (0 <= self._ref_idx < len(self._cache)):
            self.sector_bars.set_sectors([])
            return
        ref = self._cache[self._ref_idx].lap
        sectors: list[tuple[str, float]] = []
        # Show deltas for all *selected* non-reference laps, side-by-side
        # by sector. For Phase 1 we only show the FIRST selected non-ref
        # lap's per-sector delta — clarity beats density. Later phases
        # can stack multiple comparisons.
        target: Lap | None = None
        for i, lv in self._selected():
            if i != self._ref_idx:
                target = lv.lap
                break
        if target is None:
            self.sector_bars.set_sectors([])
            return
        n = min(len(ref.sectors), len(target.sectors))
        for s in range(n):
            delta = (target.sectors[s] - ref.sectors[s]) / 1000.0
            sectors.append((f"Sektor {s + 1}", delta))
        self.sector_bars.set_sectors(sectors)

    # ── Player picks + auto-download ───────────────────────────────────

    def _rebuild_player_picks(self) -> None:
        """Refresh the A/B picker combos. Items get a tag so the user
        can see at a glance which laps already have a video downloaded."""
        for combo in (self.player_a_pick, self.player_b_pick):
            combo.blockSignals(True)
            combo.clear()
            combo.addItem("— kein Video —", -1)
            for i, lv in enumerate(self._cache):
                tag = "✓" if lv.avi_path else "…"
                label = f"{tag}  {self._picker_label(lv)}"
                combo.addItem(label, i)
            combo.setCurrentIndex(0)
            combo.blockSignals(False)

    def _kick_auto_download(self) -> None:
        if self._auto_dl is not None and self._auto_dl.isRunning():
            self._auto_dl.cancel()
            self._auto_dl.wait(2000)
        host = str(self._settings.value("cam/host", "192.168.4.2", str))
        port = int(self._settings.value("cam/port", 80, int))
        jobs = [(i, lv.session, lv.lap) for i, lv in enumerate(self._cache)]
        self._auto_dl = _AutoDownloadWorker(jobs, host, port)
        self._auto_dl.lap_ready.connect(self._on_lap_video_ready)
        self._auto_dl.progress.connect(self._mw.set_status)
        self._auto_dl.finished_all.connect(
            lambda: self._mw.set_status("Auto-Download fertig")
        )
        self._auto_dl.start()
        self._mw.set_status("Lade Videos vom Kameramodul …")

    def _on_lap_video_ready(self, cache_idx: int, avi_path: str,
                            telem) -> None:  # noqa: ANN001
        if not (0 <= cache_idx < len(self._cache)):
            return
        lv = self._cache[cache_idx]
        lv.avi_path = Path(avi_path)
        lv.telemetry = telem if isinstance(telem, Telemetry) else None
        self._rebuild_player_picks()
        # First video arrival: auto-show the video region. The user can
        # still hide it via the side-panel toggle if they want maximum
        # chart real estate.
        if not self.cb_video_visible.isChecked():
            self.cb_video_visible.setChecked(True)
        # Auto-load reference lap into Player A on first arrival
        if (lv is self._cache[self._ref_idx]
                and self.player_a_pick.findData(cache_idx) >= 0):
            idx = self.player_a_pick.findData(cache_idx)
            self.player_a_pick.setCurrentIndex(idx)

    def _on_video_visibility_toggled(self, on: bool) -> None:
        self._video_region.setVisible(on)
        if not on:
            # Stop both players to release the codec; no point spinning
            # libVLC for an invisible widget.
            self.player_a.stop()
            self.player_b.stop()

    def _load_into_player(self, combo, player: VideoPlayer,  # noqa: ANN001
                          hud: HudOverlay) -> None:
        cache_idx = combo.currentData()
        if cache_idx is None or cache_idx < 0:
            player.stop()
            hud.set_telemetry(None)
            return
        if not (0 <= cache_idx < len(self._cache)):
            return
        lv = self._cache[cache_idx]
        if lv.avi_path is None:
            return
        player.play_path(str(lv.avi_path))
        hud.set_telemetry(lv.telemetry)

    # ── Dual mode + transport ──────────────────────────────────────────

    def _set_dual(self, on: bool) -> None:
        self.stage_b.setVisible(on)
        self.btn_play_b.setVisible(on)
        self.scrub_b.setVisible(on)
        self.lbl_time_b.setVisible(on)
        self.player_b_pick.setVisible(on)
        if on:
            self.video_split.setSizes([1, 1])
        else:
            self.video_split.setSizes([1, 0])
            self.player_b.stop()

    def _sync_active(self) -> bool:
        return self.cb_dual.isChecked() and self.cb_sync.isChecked()

    def _toggle_play_a(self) -> None:
        if self.player_a.is_playing():
            self.player_a.set_paused(True)
            if self._sync_active():
                self.player_b.set_paused(True)
        else:
            self.player_a.set_paused(False)
            if self._sync_active():
                self.player_b.set_paused(False)

    def _toggle_play_b(self) -> None:
        if self.player_b.is_playing():
            self.player_b.set_paused(True)
        else:
            self.player_b.set_paused(False)

    def _on_player_a_pos(self, ms: int) -> None:
        if not self.scrub_a.isSliderDown():
            self.scrub_a.blockSignals(True)
            self.scrub_a.setValue(ms)
            self.scrub_a.blockSignals(False)
        self.lbl_time_a.setText(_fmt_ms(ms))
        self.hud_a.set_position_ms(ms)
        self.btn_play_a.setText("⏸" if self.player_a.is_playing() else "▶")
        if self._sync_active() and not self._dual_busy:
            b_pos = self.player_b.position_ms()
            if b_pos >= 0 and abs(b_pos - ms) > 250:
                self._dual_busy = True
                self.player_b.set_position_ms(ms)
                self._dual_busy = False

    def _on_player_b_pos(self, ms: int) -> None:
        if not self.scrub_b.isSliderDown():
            self.scrub_b.blockSignals(True)
            self.scrub_b.setValue(ms)
            self.scrub_b.blockSignals(False)
        self.lbl_time_b.setText(_fmt_ms(ms))
        self.hud_b.set_position_ms(ms)
        self.btn_play_b.setText("⏸" if self.player_b.is_playing() else "▶")

    def _on_player_a_dur(self, ms: int) -> None:
        if ms > 0:
            self.scrub_a.setRange(0, ms)

    def _on_player_b_dur(self, ms: int) -> None:
        if ms > 0:
            self.scrub_b.setRange(0, ms)

    def _on_scrub_a(self, ms: int) -> None:
        self.player_a.set_position_ms(ms)
        self.hud_a.set_position_ms(ms)
        if self._sync_active():
            self.player_b.set_position_ms(ms)
            self.hud_b.set_position_ms(ms)

    def _on_scrub_b(self, ms: int) -> None:
        self.player_b.set_position_ms(ms)
        self.hud_b.set_position_ms(ms)

    # ── exports + map cache ────────────────────────────────────────────

    def _export_csv(self) -> None:
        sel = self._selected()
        if not sel:
            QMessageBox.information(self, "Export",
                                    "Keine Lap ausgewählt.")
            return
        if len(sel) == 1:
            i, lv = sel[0]
            default = f"lap_{lv.lap.number}.csv"
            path, _ = QFileDialog.getSaveFileName(
                self, "Lap als CSV speichern", default, "CSV (*.csv)"
            )
            if not path:
                return
            try:
                export_lap_csv(lv.lap, Path(path))
            except OSError as e:
                QMessageBox.warning(self, "Fehler", str(e))
                return
            self._mw.set_status(f"Exportiert: {path}")
            return
        # Multiple laps — pick a folder and emit one CSV per lap
        folder = QFileDialog.getExistingDirectory(
            self, "Zielordner für CSVs wählen"
        )
        if not folder:
            return
        out_dir = Path(folder)
        n = 0
        for _, lv in sel:
            try:
                export_lap_csv(lv.lap, out_dir / f"lap_{lv.lap.number}.csv")
                n += 1
            except OSError:
                continue
        self._mw.set_status(f"{n} CSV(s) exportiert nach {folder}")

    def _cache_map(self) -> None:
        """Compute bbox from selected laps' track points + small pad."""
        sel = self._selected()
        if not sel:
            QMessageBox.information(self, "Karte cachen",
                                    "Keine Lap ausgewählt.")
            return
        lats: list[float] = []
        lons: list[float] = []
        for _, lv in sel:
            for p in lv.lap.points:
                lats.append(p.lat); lons.append(p.lon)
        if len(lats) < 2:
            QMessageBox.information(
                self, "Karte cachen",
                "Lap hat zu wenig Track-Punkte für eine Region.",
            )
            return
        north, south = max(lats), min(lats)
        east, west = max(lons), min(lons)
        pad_lat = max(0.003, (north - south) * 0.20)
        pad_lon = max(0.003, (east - west) * 0.20)
        bounds = TileBounds(
            north=north + pad_lat, south=south - pad_lat,
            east=east + pad_lon, west=west - pad_lon,
        )
        TilePrefetchDialog(self, bounds, zoom_min=12, zoom_max=17).exec()

    def _on_cursor_moved(self, distance_m: float) -> None:
        if not self._cache or not (0 <= self._ref_idx < len(self._cache)):
            return
        ref = self._cache[self._ref_idx]
        # Distances on the reference lap are monotonically increasing — bisect.
        idx = bisect.bisect_left(ref.channels.distance_m, distance_m)
        if idx >= len(ref.lap.points):
            idx = len(ref.lap.points) - 1
        if idx < 0:
            return
        p = ref.lap.points[idx]
        self.map.set_cursor(p.lat, p.lon)
        # Drive each loaded video to the same on-track distance. We map
        # distance_m → lap_ms in *that player's* lap (not the reference)
        # so two laps of different speed land at the same physical spot
        # on the track, which is the whole point of side-by-side video.
        self._scrub_player_to_distance(self.player_a_pick, self.player_a,
                                       self.hud_a, distance_m)
        if self.cb_dual.isChecked():
            self._scrub_player_to_distance(self.player_b_pick, self.player_b,
                                           self.hud_b, distance_m)

    def _scrub_player_to_distance(self, combo, player: VideoPlayer,  # noqa: ANN001
                                  hud: HudOverlay,
                                  distance_m: float) -> None:
        cache_idx = combo.currentData()
        if cache_idx is None or cache_idx < 0:
            return
        if not (0 <= cache_idx < len(self._cache)):
            return
        lv = self._cache[cache_idx]
        if lv.avi_path is None or not lv.lap.points:
            return
        # Find the matching point in *this* lap (not the ref) — same
        # physical track distance, but in the lap that's actually
        # playing in this player.
        i = bisect.bisect_left(lv.channels.distance_m, distance_m)
        if i >= len(lv.lap.points):
            i = len(lv.lap.points) - 1
        if i < 0:
            return
        ms = lv.lap.points[i].lap_ms
        player.set_position_ms(int(ms))
        hud.set_position_ms(int(ms))
