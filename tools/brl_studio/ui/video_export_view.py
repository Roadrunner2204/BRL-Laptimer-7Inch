"""
Video-Export view — design a free overlay, then burn it onto a video.

Replaces the old VideoView. Playback now lives in AnalyseView (where it
sits next to the charts/map for actual telemetry analysis); this tab is
exclusively a *designer* + *export* surface.

Layout:
    ┌─────────────────────────────────────────────────────────────────┐
    │ [Lokale AVI]  [Telemetrie]  [Default] [Laden…] [Speichern…] [📤]│
    ├──────────┬──────────────────────────────┬───────────────────────┤
    │ Add:     │                              │ Eigenschaften         │
    │  Wert    │                              │  X / Y / W / H        │
    │  Label   │      Designer (1920×1080)    │  Farben / Schrift     │
    │  Box     │                              │  Field / Text / Bild  │
    │  Linie   │                              │                       │
    │  Bild    │                              │                       │
    │  ✕       │                              │                       │
    ├──────────┴──────────────────────────────┴───────────────────────┤
    │ ◀ Preview-Scrubber: ms 12345 / 95400  ▶ [Live-Preview]          │
    └─────────────────────────────────────────────────────────────────┘

The preview shows render_overlay_image() at the chosen recording-time —
exact same painter that ffmpeg will use during the actual export, so
WYSIWYG holds.
"""

from __future__ import annotations

import json
from pathlib import Path

from PyQt6.QtCore import Qt, QThread, pyqtSignal
from PyQt6.QtGui import QPixmap
from PyQt6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QProgressDialog,
    QPushButton,
    QSlider,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from core.export import Mp4Options, export_mp4, find_ffmpeg, render_overlay_image
from core.lvt_format import WIDGET_DEFAULTS, empty_lvt
from core.telemetry import Telemetry, load_telemetry_file
from ui.widgets.lvt_designer import LvtDesigner
from ui.widgets.lvt_properties import LvtProperties


# Video overlays default to 1920×1080 — matches typical export size and
# keeps a 1:1 mapping during preview/render.
OVL_W = 1920
OVL_H = 1080


def _fmt_ms(ms: int) -> str:
    s = max(0, ms) / 1000.0
    m, s = divmod(s, 60.0)
    return f"{int(m)}:{s:06.3f}"


class _LivePreview(QLabel):
    """Renders render_overlay_image() at preview_ms whenever it changes."""

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(320, 180)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setStyleSheet("background-color: #0d0f12; border: 1px solid #3a3f48;")
        self._template: dict | None = None
        self._telemetry: Telemetry | None = None
        self._ms: int = 0

    def set_template(self, t: dict | None) -> None:
        self._template = t
        self._refresh()

    def set_telemetry(self, t: Telemetry | None) -> None:
        self._telemetry = t
        self._refresh()

    def set_ms(self, ms: int) -> None:
        self._ms = max(0, ms)
        self._refresh()

    def resizeEvent(self, evt) -> None:  # noqa: ANN001
        super().resizeEvent(evt)
        self._refresh()

    def _refresh(self) -> None:
        if self._template is None:
            self.setText("Kein Layout")
            return
        # Render at design resolution, then scale to widget size.
        img = render_overlay_image(self._template, self._telemetry,
                                   self._ms, OVL_W, OVL_H)
        pix = QPixmap.fromImage(img).scaled(
            self.size(),
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        self.setPixmap(pix)


class _Mp4Worker(QThread):
    progress = pyqtSignal(int, int, str)
    finished_ok = pyqtSignal(str)
    finished_err = pyqtSignal(str)

    def __init__(self, avi: Path, dest: Path,
                 telemetry: Telemetry | None,
                 options: Mp4Options) -> None:
        super().__init__()
        self.avi = avi
        self.dest = dest
        self.telemetry = telemetry
        self.options = options
        self._cancel = False

    def cancel(self) -> None:
        self._cancel = True

    def run(self) -> None:
        try:
            export_mp4(
                self.avi, self.dest, self.telemetry, self.options,
                progress_cb=lambda d, t, m: (
                    self.progress.emit(d, t, m)
                    if not self._cancel else None
                ),
            )
            if self._cancel:
                self.finished_err.emit("Abgebrochen")
                return
            self.finished_ok.emit(str(self.dest))
        except (RuntimeError, OSError) as e:
            self.finished_err.emit(str(e))


class VideoExportView(QWidget):
    def __init__(self, main_window) -> None:  # noqa: ANN001
        super().__init__()
        self._mw = main_window
        self._sample_avi: Path | None = None
        self._telemetry: Telemetry | None = None
        self._overlay_path: Path | None = None
        self._duration_ms = 0

        # ── Designer with video-frame canvas ───────────────────────────
        self.designer = LvtDesigner(canvas_w=OVL_W, canvas_h=OVL_H)
        # Start with an empty 1920×1080 doc instead of the 1024×600 default.
        empty = empty_lvt()
        empty["width"] = OVL_W
        empty["height"] = OVL_H
        self.designer.set_doc(empty)
        self.designer.selection_changed.connect(self._on_selection)
        self.designer.doc_changed.connect(self._on_doc_changed)

        # ── Properties panel ───────────────────────────────────────────
        self.props = LvtProperties()
        self.props.changed.connect(
            lambda patch: self.designer.update_selected(**patch)
        )

        # ── Preview ────────────────────────────────────────────────────
        self.preview = _LivePreview()
        self.preview.set_template(self.designer.doc())

        self.scrub = QSlider(Qt.Orientation.Horizontal)
        self.scrub.setRange(0, 0)
        self.scrub.valueChanged.connect(self._on_scrub)
        self.lbl_scrub = QLabel("0:00.000 / 0:00.000")
        self.lbl_scrub.setMinimumWidth(180)
        self.lbl_scrub.setStyleSheet("font-family: monospace;")

        # ── Toolbar (left) ─────────────────────────────────────────────
        tb = QVBoxLayout()
        tb.addWidget(QLabel("Hinzufügen"))
        for kind, label in (("value", "Wertfeld"), ("label", "Label"),
                            ("rect", "Box"), ("line", "Linie"),
                            ("image", "Bild")):
            btn = QPushButton(f"+ {label}")
            btn.clicked.connect(
                lambda _=False, k=kind: self.designer.add_widget(WIDGET_DEFAULTS[k])
            )
            tb.addWidget(btn)
        tb.addSpacing(12)
        btn_del = QPushButton("🗑 Auswahl löschen")
        btn_del.clicked.connect(self.designer.remove_selected)
        tb.addWidget(btn_del)
        tb.addStretch(1)
        tb_w = QWidget(); tb_w.setLayout(tb); tb_w.setMaximumWidth(180)

        # ── Top action bar ─────────────────────────────────────────────
        self.btn_avi = QPushButton("🎬 Sample-Video laden…")
        self.btn_avi.clicked.connect(self._pick_sample_avi)
        self.btn_telem = QPushButton("📊 Telemetrie laden…")
        self.btn_telem.clicked.connect(self._pick_telemetry)
        self.btn_default = QPushButton("Default-Overlay")
        self.btn_default.clicked.connect(self._load_default)
        self.btn_load = QPushButton("Laden…")
        self.btn_load.clicked.connect(self._load_template)
        self.btn_save = QPushButton("💾 Speichern…")
        self.btn_save.clicked.connect(self._save_template)
        self.btn_export = QPushButton("📤 Als MP4 exportieren…")
        self.btn_export.clicked.connect(self._export)

        actions = QHBoxLayout()
        actions.addWidget(self.btn_avi)
        actions.addWidget(self.btn_telem)
        actions.addSpacing(20)
        actions.addWidget(self.btn_default)
        actions.addWidget(self.btn_load)
        actions.addWidget(self.btn_save)
        actions.addStretch(1)
        actions.addWidget(self.btn_export)

        # ── Right column: properties + preview ─────────────────────────
        right = QVBoxLayout()
        right.addWidget(QLabel("Eigenschaften"))
        right.addWidget(self.props, stretch=1)
        right.addWidget(QLabel("Live-Preview"))
        right.addWidget(self.preview, stretch=1)
        right_w = QWidget(); right_w.setLayout(right); right_w.setMaximumWidth(380)

        # ── Compose ────────────────────────────────────────────────────
        body = QSplitter(Qt.Orientation.Horizontal)
        body.addWidget(tb_w)
        body.addWidget(self.designer)
        body.addWidget(right_w)
        body.setStretchFactor(0, 0)
        body.setStretchFactor(1, 1)
        body.setStretchFactor(2, 0)

        scrub_row = QHBoxLayout()
        scrub_row.addWidget(QLabel("Preview"))
        scrub_row.addWidget(self.scrub, stretch=1)
        scrub_row.addWidget(self.lbl_scrub)

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(6)
        root.addLayout(actions)
        root.addWidget(body, stretch=1)
        root.addLayout(scrub_row)

    # ── Sources ────────────────────────────────────────────────────────

    def _pick_sample_avi(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Sample-Video", "",
            "Videos (*.avi *.mp4 *.mkv);;Alle Dateien (*.*)",
        )
        if not path:
            return
        self._sample_avi = Path(path)
        # Try to find a sibling NDJSON automatically
        for cand in (self._sample_avi.with_suffix(".ndjson"),
                     self._sample_avi.parent / "telemetry.ndjson"):
            if cand.exists():
                try:
                    self._telemetry = load_telemetry_file(cand)
                    self._on_telemetry_loaded()
                    break
                except (OSError, ValueError):
                    continue
        self._mw.set_status(f"Sample: {self._sample_avi.name}")

    def _pick_telemetry(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Telemetrie (.ndjson)", "",
            "NDJSON (*.ndjson);;Alle Dateien (*.*)",
        )
        if not path:
            return
        try:
            self._telemetry = load_telemetry_file(Path(path))
        except (OSError, ValueError) as e:
            QMessageBox.warning(self, "Fehler", str(e))
            return
        self._on_telemetry_loaded()

    def _on_telemetry_loaded(self) -> None:
        if self._telemetry is None:
            return
        self.preview.set_telemetry(self._telemetry)
        self._duration_ms = self._telemetry.duration_ms or 60000
        self.scrub.setRange(0, self._duration_ms)
        self.scrub.setValue(0)
        self.lbl_scrub.setText(f"0:00.000 / {_fmt_ms(self._duration_ms)}")
        self._mw.set_status(
            f"Telemetrie: {len(self._telemetry.gps)} GPS, "
            f"{len(self._telemetry.obd)} OBD, "
            f"{len(self._telemetry.laps)} Laps"
        )

    # ── Template I/O ───────────────────────────────────────────────────

    def _load_default(self) -> None:
        """A reasonable starter overlay — a few common widgets pre-placed."""
        from core.dash_config import (FIELD_LAPTIME, FIELD_LAP_NR,
                                      FIELD_RPM, FIELD_SPEED, FIELD_THROTTLE)
        doc = empty_lvt()
        doc["width"] = OVL_W
        doc["height"] = OVL_H
        doc["widgets"] = [
            {"type": "value", "x": 60, "y": 60, "w": 320, "h": 140,
             "field": FIELD_SPEED, "font_pt": 96, "color": "#FFFFFF",
             "align": "left"},
            {"type": "label", "x": 380, "y": 130, "w": 120, "h": 40,
             "text": "km/h", "font_pt": 36, "color": "#cccccc",
             "align": "left"},
            {"type": "value", "x": 1500, "y": 60, "w": 360, "h": 90,
             "field": FIELD_LAPTIME, "font_pt": 64, "color": "#FFFFFF",
             "align": "right"},
            {"type": "value", "x": 1500, "y": 150, "w": 360, "h": 50,
             "field": FIELD_LAP_NR, "font_pt": 32, "color": "#cccccc",
             "align": "right"},
            {"type": "value", "x": 60, "y": 940, "w": 240, "h": 100,
             "field": FIELD_RPM, "font_pt": 64, "color": "#FFFFFF",
             "align": "left"},
            {"type": "value", "x": 1620, "y": 940, "w": 240, "h": 100,
             "field": FIELD_THROTTLE, "font_pt": 48, "color": "#2ECC71",
             "align": "right"},
        ]
        self.designer.set_doc(doc)
        self.preview.set_template(self.designer.doc())

    def _load_template(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Overlay öffnen", "", "Overlay (*.ovl *.lvt *.json)"
        )
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            QMessageBox.warning(self, "Fehler", str(e))
            return
        if not isinstance(doc, dict):
            QMessageBox.warning(self, "Fehler", "Kein Overlay-Objekt.")
            return
        self.designer.set_doc(doc)
        self.preview.set_template(self.designer.doc())
        self._overlay_path = Path(path)
        self._mw.set_status(f"Overlay: {path}")

    def _save_template(self) -> None:
        default = (str(self._overlay_path)
                   if self._overlay_path is not None else "overlay.ovl")
        path, _ = QFileDialog.getSaveFileName(
            self, "Overlay speichern", default, "Overlay (*.ovl)"
        )
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(self.designer.doc(), f, ensure_ascii=False, indent=2)
        except OSError as e:
            QMessageBox.warning(self, "Fehler", str(e))
            return
        self._overlay_path = Path(path)
        self._mw.set_status(f"Gespeichert: {path}")

    # ── Designer events ────────────────────────────────────────────────

    def _on_selection(self, w: dict | None) -> None:
        self.props.set_widget(w)

    def _on_doc_changed(self) -> None:
        self.preview.set_template(self.designer.doc())

    def _on_scrub(self, ms: int) -> None:
        self.preview.set_ms(ms)
        self.lbl_scrub.setText(f"{_fmt_ms(ms)} / {_fmt_ms(self._duration_ms)}")

    # ── Export ─────────────────────────────────────────────────────────

    def _export(self) -> None:
        if find_ffmpeg() is None:
            QMessageBox.warning(
                self, "ffmpeg fehlt",
                "ffmpeg wurde nicht gefunden.\n\n"
                "Installiere ffmpeg (https://ffmpeg.org) und stelle sicher, "
                "dass `ffmpeg` im PATH liegt — oder lege ffmpeg/ffmpeg.exe "
                "neben BRL-Studio.exe.",
            )
            return
        if self._sample_avi is None:
            path, _ = QFileDialog.getOpenFileName(
                self, "Quell-AVI wählen", "",
                "Videos (*.avi *.mp4 *.mkv);;Alle Dateien (*.*)",
            )
            if not path:
                return
            self._sample_avi = Path(path)
        if self._telemetry is None:
            QMessageBox.information(
                self, "Telemetrie fehlt",
                "Erst eine Telemetrie-NDJSON laden, sonst zeigt das Overlay "
                "nur Platzhalter (—) für jeden Wert.",
            )
            return

        dest, _ = QFileDialog.getSaveFileName(
            self, "MP4 speichern als…",
            str(self._sample_avi.with_suffix(".overlay.mp4")),
            "MP4 (*.mp4)",
        )
        if not dest:
            return

        opts = Mp4Options(
            burn_hud=True,
            overlay_template=self.designer.doc(),
        )
        worker = _Mp4Worker(self._sample_avi, Path(dest),
                            self._telemetry, opts)
        prog = QProgressDialog("Initialisiere…", "Abbrechen", 0, 1, self)
        prog.setWindowTitle("MP4-Export")
        prog.setWindowModality(Qt.WindowModality.WindowModal)
        prog.setMinimumDuration(0)
        prog.setAutoClose(False); prog.setAutoReset(False)

        worker.progress.connect(
            lambda d, t, m: (prog.setMaximum(t or 1), prog.setValue(d),
                             prog.setLabelText(m))
        )
        worker.finished_ok.connect(
            lambda p: (prog.close(),
                       self._mw.set_status(f"MP4 fertig: {p}"),
                       QMessageBox.information(self, "Fertig",
                                               f"Exportiert: {p}"))
        )
        worker.finished_err.connect(
            lambda m: (prog.close(),
                       QMessageBox.warning(self, "Export-Fehler", m))
        )
        prog.canceled.connect(worker.cancel)
        worker.start()
