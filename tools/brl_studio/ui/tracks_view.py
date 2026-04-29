"""
Track editor — Phase 4.

Edits the laptimer's user-track set + lets the user create new tracks.
The built-in DB is read-only by design (mirrors the Display behaviour).

Workflow:
    1. Load tracks from Display (`GET /tracks`) or open a JSON file.
    2. Pick a track on the left → map shows S/F line + sector markers,
       both draggable. Right panel exposes name/country/length/circuit
       + sector list (table).
    3. Edit by:
        - dragging markers on the map (the map bridge updates the model)
        - clicking on the map after pressing one of the "Setze …" buttons
          (S/F-A, S/F-B, neuer Sektor)
        - editing fields directly in the right panel
    4. Save: "Lokal speichern" → JSON file picker; "Zum Display senden"
       → `POST /track` to the laptimer.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path

from PyQt6.QtCore import QSettings, Qt, QThread, pyqtSignal
from PyQt6.QtGui import QDoubleValidator
from PyQt6.QtWidgets import (
    QCheckBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from core.http_client import DisplayClient, Endpoint, probe
from core.nominatim import NominatimWorker, SearchHit
from core.tile_server import TileBounds
from ui.widgets.map import LeafletMap
from ui.widgets.tile_prefetch_dialog import TilePrefetchDialog


def _new_track() -> dict:
    return {
        "name": "Neuer Track",
        "country": "",
        "length_km": 0.0,
        "is_circuit": True,
        "sf": [47.7834, 13.1846, 47.7832, 13.1846],
        "sectors": [],
    }


class _TracksFetchWorker(QThread):
    finished_ok = pyqtSignal(list)
    finished_err = pyqtSignal(str)

    def __init__(self, host: str, port: int) -> None:
        super().__init__()
        self.host = host
        self.port = port

    def run(self) -> None:
        try:
            ep = Endpoint(host=self.host, port=self.port)
            if not probe(ep):
                self.finished_err.emit(f"Display nicht erreichbar ({self.host})")
                return
            tracks = DisplayClient(ep).list_tracks()
            self.finished_ok.emit(tracks)
        except Exception as e:  # noqa: BLE001
            self.finished_err.emit(str(e))


class _TrackDetailWorker(QThread):
    """Lazy-load the full TrackDef for one track when the user picks it."""

    finished_ok = pyqtSignal(int, object)   # (track_idx, track_dict)
    finished_err = pyqtSignal(int, str)

    def __init__(self, host: str, port: int, idx: int) -> None:
        super().__init__()
        self.host = host
        self.port = port
        self.idx = idx

    def run(self) -> None:
        try:
            ep = Endpoint(host=self.host, port=self.port)
            doc = DisplayClient(ep).get_track(self.idx)
            self.finished_ok.emit(self.idx, doc)
        except Exception as e:  # noqa: BLE001
            self.finished_err.emit(self.idx, str(e))


class _TrackUploadWorker(QThread):
    finished_ok = pyqtSignal()
    finished_err = pyqtSignal(str)

    def __init__(self, host: str, port: int, track: dict) -> None:
        super().__init__()
        self.host = host
        self.port = port
        self.track = track

    def run(self) -> None:
        try:
            ep = Endpoint(host=self.host, port=self.port)
            DisplayClient(ep).upload_track(self.track)
            self.finished_ok.emit()
        except Exception as e:  # noqa: BLE001
            self.finished_err.emit(str(e))


class TracksView(QWidget):
    def __init__(self, main_window) -> None:  # noqa: ANN001
        super().__init__()
        self._mw = main_window
        self._settings = QSettings()
        self._tracks: list[dict] = []
        self._current: dict | None = None
        self._pick_mode: str = "off"     # what the next map click sets
        self._suppress_form_signals = False

        # ── Left: track list + new/load buttons ────────────────────────
        self.track_list = QListWidget()
        self.track_list.itemSelectionChanged.connect(self._on_track_selected)

        self.btn_load_display = QPushButton("Vom Display laden")
        self.btn_load_display.clicked.connect(self._load_from_display)
        self.btn_load_file = QPushButton("Aus Datei laden…")
        self.btn_load_file.clicked.connect(self._load_from_file)
        self.btn_new = QPushButton("➕ Neu")
        self.btn_new.clicked.connect(self._new_track)
        self.btn_delete = QPushButton("🗑 Löschen")
        self.btn_delete.clicked.connect(self._delete_track)

        left_btns = QHBoxLayout()
        left_btns.addWidget(self.btn_new)
        left_btns.addWidget(self.btn_delete)

        left = QVBoxLayout()
        left.addWidget(QLabel("Tracks"))
        left.addWidget(self.track_list, stretch=1)
        left.addLayout(left_btns)
        left.addWidget(self.btn_load_display)
        left.addWidget(self.btn_load_file)
        left_w = QWidget()
        left_w.setLayout(left)
        left_w.setMaximumWidth(240)

        # ── Middle: map + search bar ───────────────────────────────────
        self.map = LeafletMap()
        self.map.bridge.map_clicked.connect(self._on_map_click)
        self.map.bridge.sector_dragged.connect(self._on_sector_dragged)
        self.map.bridge.sf_dragged.connect(self._on_sf_dragged)
        self.map.bridge.position_received.connect(self._on_position_received)
        self.map.bridge.geolocation_error.connect(self._on_geolocation_error)

        self.search_input = QLineEdit()
        self.search_input.setPlaceholderText("Adresse / Track / Ort suchen…")
        self.search_input.returnPressed.connect(self._do_search)
        self.btn_search = QPushButton("🔍")
        self.btn_search.setFixedWidth(40)
        self.btn_search.clicked.connect(self._do_search)
        self.btn_my_position = QPushButton("📍 Meine Position")
        self.btn_my_position.clicked.connect(self.map.request_position)

        search_row = QHBoxLayout()
        search_row.addWidget(self.search_input, stretch=1)
        search_row.addWidget(self.btn_search)
        search_row.addWidget(self.btn_my_position)

        # Search results list — hidden until a query returns hits
        self.search_results = QListWidget()
        self.search_results.setMaximumHeight(140)
        self.search_results.itemClicked.connect(self._on_search_hit_clicked)
        self.search_results.setVisible(False)
        self._search_hits: list[SearchHit] = []
        self._search_worker: NominatimWorker | None = None

        map_col = QVBoxLayout()
        map_col.setContentsMargins(0, 0, 0, 0)
        map_col.setSpacing(4)
        map_col.addLayout(search_row)
        map_col.addWidget(self.search_results)
        map_col.addWidget(self.map, stretch=1)
        map_col_w = QWidget(); map_col_w.setLayout(map_col)

        # ── Right: form + sectors table + save row ─────────────────────
        self.f_name = QLineEdit()
        self.f_country = QLineEdit()
        self.f_length = QLineEdit()
        self.f_length.setValidator(QDoubleValidator(0.0, 999.0, 3))
        self.f_circuit = QCheckBox("Rundkurs")
        for w in (self.f_name, self.f_country, self.f_length):
            w.editingFinished.connect(self._on_form_edited)
        self.f_circuit.toggled.connect(self._on_form_edited)

        form = QFormLayout()
        form.addRow("Name", self.f_name)
        form.addRow("Land", self.f_country)
        form.addRow("Länge (km)", self.f_length)
        form.addRow("", self.f_circuit)

        self.btn_pick_sf1 = QPushButton("S/F-A auf Karte setzen")
        self.btn_pick_sf2 = QPushButton("S/F-B auf Karte setzen")
        self.btn_add_sector = QPushButton("➕ Neuer Sektor (Karte)")
        self.btn_pick_sf1.clicked.connect(lambda: self._set_pick_mode("sf1"))
        self.btn_pick_sf2.clicked.connect(lambda: self._set_pick_mode("sf2"))
        self.btn_add_sector.clicked.connect(
            lambda: self._set_pick_mode("sector_add")
        )

        pick_row = QHBoxLayout()
        pick_row.addWidget(self.btn_pick_sf1)
        pick_row.addWidget(self.btn_pick_sf2)
        pick_row.addWidget(self.btn_add_sector)

        self.sec_table = QTableWidget(0, 4)
        self.sec_table.setHorizontalHeaderLabels(["Name", "Lat", "Lon", ""])
        self.sec_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self.sec_table.verticalHeader().setVisible(False)
        self.sec_table.itemChanged.connect(self._on_sector_edit)

        self.btn_save_local = QPushButton("💾 Lokal speichern…")
        self.btn_save_local.clicked.connect(self._save_local)
        self.btn_send_display = QPushButton("📡 Zum Display senden")
        self.btn_send_display.clicked.connect(self._send_to_display)
        self.btn_cache_map = QPushButton("🗺 Karte cachen")
        self.btn_cache_map.setToolTip(
            "Lädt OSM-Tiles für diesen Track in den Offline-Cache, damit "
            "die Karte am Laptimer-WiFi auch ohne Internet funktioniert."
        )
        self.btn_cache_map.clicked.connect(self._cache_map)

        save_row = QHBoxLayout()
        save_row.addWidget(self.btn_save_local)
        save_row.addWidget(self.btn_cache_map)
        save_row.addStretch(1)
        save_row.addWidget(self.btn_send_display)

        right = QVBoxLayout()
        right.addLayout(form)
        right.addSpacing(6)
        right.addLayout(pick_row)
        right.addWidget(QLabel("Sektoren"))
        right.addWidget(self.sec_table, stretch=1)
        right.addLayout(save_row)
        right_w = QWidget()
        right_w.setLayout(right)

        # ── Compose ────────────────────────────────────────────────────
        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(left_w)
        split.addWidget(map_col_w)
        split.addWidget(right_w)
        split.setStretchFactor(0, 1)
        split.setStretchFactor(1, 3)
        split.setStretchFactor(2, 2)

        root = QHBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.addWidget(split)

        self._set_form_enabled(False)

    # ── Loading ────────────────────────────────────────────────────────

    def _load_from_display(self) -> None:
        host = str(self._settings.value("display/host", "192.168.4.1", str))
        port = int(self._settings.value("display/port", 80, int))
        self.btn_load_display.setEnabled(False)
        self._mw.set_status(f"Lade Tracks von {host}…")
        self._w = _TracksFetchWorker(host, port)
        self._w.finished_ok.connect(self._on_tracks_ok)
        self._w.finished_err.connect(self._on_tracks_err)
        self._w.finished.connect(
            lambda: self.btn_load_display.setEnabled(True)
        )
        self._w.start()

    def _on_tracks_ok(self, tracks: list) -> None:
        self._tracks = list(tracks)
        self._refresh_track_list()
        self._mw.set_status(f"{len(tracks)} Track(s) geladen")

    def _on_tracks_err(self, msg: str) -> None:
        QMessageBox.warning(self, "Fehler", msg)

    def _load_from_file(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "Track-JSON wählen", "", "JSON (*.json)"
        )
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            QMessageBox.warning(self, "Fehler", str(e))
            return
        if isinstance(doc, list):
            self._tracks = doc
        elif isinstance(doc, dict):
            self._tracks.append(doc)
        self._refresh_track_list()

    def _refresh_track_list(self) -> None:
        self.track_list.blockSignals(True)
        self.track_list.clear()
        for t in self._tracks:
            it = QListWidgetItem(t.get("name", "—"))
            self.track_list.addItem(it)
        self.track_list.blockSignals(False)
        if self._tracks:
            self.track_list.setCurrentRow(0)

    def _new_track(self) -> None:
        self._tracks.append(_new_track())
        self._refresh_track_list()
        self.track_list.setCurrentRow(len(self._tracks) - 1)

    def _delete_track(self) -> None:
        rows = self.track_list.selectionModel().selectedRows()
        if not rows:
            return
        idx = rows[0].row()
        if idx < 0 or idx >= len(self._tracks):
            return
        del self._tracks[idx]
        self._current = None
        self._refresh_track_list()
        self.map.set_track_def(None)
        self._set_form_enabled(False)

    # ── Selection → editor sync ────────────────────────────────────────

    def _on_track_selected(self) -> None:
        rows = self.track_list.selectionModel().selectedRows()
        if not rows:
            self._current = None
            self.map.set_track_def(None)
            self._set_form_enabled(False)
            return
        idx = rows[0].row()
        if idx < 0 or idx >= len(self._tracks):
            return
        self._current = self._tracks[idx]
        # /tracks gives us only stubs (name + country + length etc). The
        # heavy bits — S/F line + sectors — only come via /track/<idx>.
        # Fetch on demand the first time the user picks a track. Keep the
        # detail blob attached to the same dict so subsequent picks skip
        # the round-trip.
        needs_detail = ("sf" not in self._current
                        and self._current.get("index") is not None
                        and "_local_only" not in self._current)
        if needs_detail:
            self._kick_track_detail_load(self._current)
        self._populate_form()
        self.map.set_track_def(self._current)
        self._set_pick_mode("off")
        self._set_form_enabled(True)

    def _kick_track_detail_load(self, stub: dict) -> None:
        """Fire a worker to GET /track/<idx>. UI keeps showing the stub
        in the meantime; once details arrive we merge them in and refresh."""
        host = str(self._settings.value("display/host", "192.168.4.1", str))
        port = int(self._settings.value("display/port", 80, int))
        idx = int(stub.get("index", -1))
        if idx < 0:
            return
        self._mw.set_status(f"Lade Track-Details für '{stub.get('name', idx)}'…")
        self._dw = _TrackDetailWorker(host, port, idx)
        self._dw.finished_ok.connect(self._on_track_detail_ok)
        self._dw.finished_err.connect(self._on_track_detail_err)
        self._dw.start()

    def _on_track_detail_ok(self, idx: int, doc: object) -> None:
        if not isinstance(doc, dict):
            return
        # Find the matching stub in _tracks (same index)
        for i, t in enumerate(self._tracks):
            if int(t.get("index", -2)) == idx:
                self._tracks[i].update(doc)
                # If the user is still on this track, re-render
                if self._current is t or (
                    self._current is not None
                    and int(self._current.get("index", -3)) == idx
                ):
                    self._current = self._tracks[i]
                    self._populate_form()
                    self.map.set_track_def(self._current)
                break
        self._mw.set_status("Track-Details geladen")

    def _on_track_detail_err(self, idx: int, msg: str) -> None:
        self._mw.set_status(f"Track {idx}: {msg}")

    # ── Search + position ─────────────────────────────────────────────

    def _do_search(self) -> None:
        q = self.search_input.text().strip()
        if not q:
            self.search_results.setVisible(False)
            return
        self.btn_search.setEnabled(False)
        self._mw.set_status(f"Suche '{q}'…")
        self._search_worker = NominatimWorker(q)
        self._search_worker.finished_ok.connect(self._on_search_ok)
        self._search_worker.finished_err.connect(self._on_search_err)
        self._search_worker.finished.connect(
            lambda: self.btn_search.setEnabled(True)
        )
        self._search_worker.start()

    def _on_search_ok(self, hits: list) -> None:
        self._search_hits = list(hits)
        self.search_results.clear()
        if not hits:
            self.search_results.setVisible(False)
            self._mw.set_status("Keine Treffer")
            return
        for h in self._search_hits:
            self.search_results.addItem(QListWidgetItem(h.display))
        self.search_results.setVisible(True)
        self._mw.set_status(f"{len(hits)} Treffer")

    def _on_search_err(self, msg: str) -> None:
        QMessageBox.warning(self, "Suche", msg)
        self._mw.set_status("Suche fehlgeschlagen")

    def _on_search_hit_clicked(self, item: QListWidgetItem) -> None:
        idx = self.search_results.row(item)
        if not (0 <= idx < len(self._search_hits)):
            return
        h = self._search_hits[idx]
        # Pick a zoom that fits the bbox if Nominatim gave us one,
        # otherwise default to 14 (city/town scale).
        if h.bbox is not None:
            self.map.goto(h.lat, h.lon, 14)
        else:
            self.map.goto(h.lat, h.lon, 14)
        self._mw.set_status(f"→ {h.display}")

    def _on_position_received(self, lat: float, lon: float) -> None:
        self._mw.set_status(f"Position: {lat:.5f}, {lon:.5f}")

    def _on_geolocation_error(self, msg: str) -> None:
        QMessageBox.information(
            self, "Geolocation",
            f"Keine Position verfügbar:\n{msg}\n\n"
            "Standortdienste in Windows aktiv? "
            "Manche Hardware hat keinen GPS-Sensor — dann nutzt Windows "
            "WiFi-Triangulation, was offline nicht klappt."
        )

    def _set_form_enabled(self, on: bool) -> None:
        for w in (self.f_name, self.f_country, self.f_length, self.f_circuit,
                  self.btn_pick_sf1, self.btn_pick_sf2, self.btn_add_sector,
                  self.sec_table, self.btn_save_local, self.btn_send_display):
            w.setEnabled(on)

    def _populate_form(self) -> None:
        if self._current is None:
            return
        self._suppress_form_signals = True
        self.f_name.setText(str(self._current.get("name", "")))
        self.f_country.setText(str(self._current.get("country", "")))
        self.f_length.setText(f"{float(self._current.get('length_km', 0)):.3f}")
        self.f_circuit.setChecked(bool(self._current.get("is_circuit", True)))

        sectors = self._current.get("sectors") or []
        self.sec_table.blockSignals(True)
        self.sec_table.setRowCount(len(sectors))
        for i, s in enumerate(sectors):
            self.sec_table.setItem(
                i, 0, QTableWidgetItem(str(s.get("name", f"S{i+1}")))
            )
            self.sec_table.setItem(
                i, 1, QTableWidgetItem(f"{float(s.get('lat', 0)):.7f}")
            )
            self.sec_table.setItem(
                i, 2, QTableWidgetItem(f"{float(s.get('lon', 0)):.7f}")
            )
            del_item = QTableWidgetItem("✕")
            del_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            del_item.setFlags(Qt.ItemFlag.ItemIsEnabled
                              | Qt.ItemFlag.ItemIsSelectable)
            self.sec_table.setItem(i, 3, del_item)
        self.sec_table.blockSignals(False)
        self._suppress_form_signals = False
        # Wire delete-cell click
        try:
            self.sec_table.cellClicked.disconnect(self._on_sector_cell_click)
        except TypeError:
            pass
        self.sec_table.cellClicked.connect(self._on_sector_cell_click)

    # ── Form edits → model ─────────────────────────────────────────────

    def _on_form_edited(self) -> None:
        if self._current is None or self._suppress_form_signals:
            return
        old_name = self._current.get("name", "")
        self._current["name"] = self.f_name.text().strip()
        self._current["country"] = self.f_country.text().strip()
        try:
            self._current["length_km"] = float(self.f_length.text())
        except ValueError:
            pass
        self._current["is_circuit"] = self.f_circuit.isChecked()
        if old_name != self._current["name"]:
            self._refresh_track_list_keep_selection()

    def _refresh_track_list_keep_selection(self) -> None:
        idx = self.track_list.currentRow()
        self._refresh_track_list()
        if 0 <= idx < self.track_list.count():
            self.track_list.setCurrentRow(idx)

    def _on_sector_edit(self, item: QTableWidgetItem) -> None:
        if self._current is None or self._suppress_form_signals:
            return
        row, col = item.row(), item.column()
        sectors = self._current.setdefault("sectors", [])
        if row >= len(sectors):
            return
        try:
            if col == 0:
                sectors[row]["name"] = item.text().strip()
            elif col == 1:
                sectors[row]["lat"] = float(item.text())
            elif col == 2:
                sectors[row]["lon"] = float(item.text())
        except ValueError:
            return
        self.map.set_track_def(self._current)

    def _on_sector_cell_click(self, row: int, col: int) -> None:
        if col != 3 or self._current is None:
            return
        sectors = self._current.setdefault("sectors", [])
        if 0 <= row < len(sectors):
            del sectors[row]
            self._populate_form()
            self.map.set_track_def(self._current)

    # ── Map interaction ────────────────────────────────────────────────

    def _set_pick_mode(self, mode: str) -> None:
        self._pick_mode = mode
        self.map.set_pick_mode(mode)
        if mode == "off":
            self._mw.set_status("")
        else:
            label = {
                "sf1": "S/F-A wählen",
                "sf2": "S/F-B wählen",
                "sector_add": "Klick = neuer Sektor",
            }.get(mode, mode)
            self._mw.set_status(f"Karte: {label} (klicken)")

    def _on_map_click(self, lat: float, lon: float) -> None:
        if self._current is None or self._pick_mode == "off":
            return
        if self._pick_mode == "sf1":
            sf = self._current.setdefault("sf", [0, 0, 0, 0])
            sf[0], sf[1] = lat, lon
        elif self._pick_mode == "sf2":
            sf = self._current.setdefault("sf", [0, 0, 0, 0])
            sf[2], sf[3] = lat, lon
        elif self._pick_mode == "sector_add":
            sectors = self._current.setdefault("sectors", [])
            sectors.append({
                "name": f"S{len(sectors) + 1}",
                "lat": lat, "lon": lon,
            })
        self._populate_form()
        self.map.set_track_def(self._current)
        self._set_pick_mode("off")

    def _on_sector_dragged(self, idx: int, lat: float, lon: float) -> None:
        if self._current is None:
            return
        sectors = self._current.setdefault("sectors", [])
        if 0 <= idx < len(sectors):
            sectors[idx]["lat"] = lat
            sectors[idx]["lon"] = lon
            self._populate_form()

    def _on_sf_dragged(self, end_idx: int, lat: float, lon: float) -> None:
        if self._current is None:
            return
        sf = self._current.setdefault("sf", [0, 0, 0, 0])
        if end_idx == 0:
            sf[0], sf[1] = lat, lon
        else:
            sf[2], sf[3] = lat, lon

    # ── Save ───────────────────────────────────────────────────────────

    def _save_local(self) -> None:
        if self._current is None:
            return
        default = (self._current.get("name") or "track").replace(" ", "_") + ".json"
        path, _ = QFileDialog.getSaveFileName(
            self, "Track-JSON speichern", default, "JSON (*.json)"
        )
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(self._current, f, ensure_ascii=False, indent=2)
        except OSError as e:
            QMessageBox.warning(self, "Fehler", str(e))
            return
        self._mw.set_status(f"Gespeichert: {path}")

    def _send_to_display(self) -> None:
        if self._current is None:
            return
        host = str(self._settings.value("display/host", "192.168.4.1", str))
        port = int(self._settings.value("display/port", 80, int))
        track = copy.deepcopy(self._current)
        self.btn_send_display.setEnabled(False)
        self._mw.set_status(f"Sende '{track.get('name', '?')}' an {host}…")
        self._uw = _TrackUploadWorker(host, port, track)
        self._uw.finished_ok.connect(self._on_upload_ok)
        self._uw.finished_err.connect(self._on_upload_err)
        self._uw.finished.connect(
            lambda: self.btn_send_display.setEnabled(True)
        )
        self._uw.start()

    def _on_upload_ok(self) -> None:
        self._mw.set_status("Track ans Display gesendet ✓")
        QMessageBox.information(self, "Erfolg", "Track wurde übertragen.")

    def _on_upload_err(self, msg: str) -> None:
        QMessageBox.warning(self, "Upload-Fehler", msg)
        self._mw.set_status("Upload fehlgeschlagen")

    # ── Map pre-cache ──────────────────────────────────────────────────

    def _cache_map(self) -> None:
        """Compute a bbox from the current track's S/F + sectors, +20% pad."""
        if self._current is None:
            QMessageBox.information(self, "Karte cachen",
                                    "Erst einen Track wählen.")
            return
        pts: list[tuple[float, float]] = []
        sf = self._current.get("sf") or []
        if len(sf) >= 4:
            pts.append((float(sf[0]), float(sf[1])))
            pts.append((float(sf[2]), float(sf[3])))
        for s in self._current.get("sectors") or []:
            pts.append((float(s.get("lat", 0)), float(s.get("lon", 0))))
        if len(pts) < 2:
            QMessageBox.information(
                self, "Karte cachen",
                "Track hat zu wenig Punkte für einen sinnvollen Bereich.",
            )
            return
        lats = [p[0] for p in pts]
        lons = [p[1] for p in pts]
        north, south = max(lats), min(lats)
        east, west = max(lons), min(lons)
        # Pad by ~20 % so panning still has tiles
        pad_lat = max(0.005, (north - south) * 0.25)
        pad_lon = max(0.005, (east - west) * 0.25)
        bounds = TileBounds(
            north=north + pad_lat, south=south - pad_lat,
            east=east + pad_lon, west=west - pad_lon,
        )
        TilePrefetchDialog(self, bounds, zoom_min=12, zoom_max=17).exec()
