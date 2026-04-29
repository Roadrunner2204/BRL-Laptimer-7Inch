"""
Sessions browser — hierarchical Tracks → Sessions → Laps.

Layout (left to right):

    ┌─────────────────┬──────────────────┬──────────────────┐
    │ Tracks (tree)   │ Laps of selected │ Compare basket   │
    │  ▾ Salzburgring │    session       │  (cross-session) │
    │     └ sess_…    │                  │                  │
    │     └ sess_…    │  multi-select    │  "Analysieren →" │
    │  ▾ Hockenheim   │  + zur Auswahl   │                  │
    └─────────────────┴──────────────────┴──────────────────┘

The basket lets the user mix laps from different sessions and even
different tracks (the analyse view will warn if track names disagree).

Sessions fetched via /sessions are summary stubs (no laps); the user
must explicitly download a session before its laps can be analysed.
The "⬇ Download" button does that and replaces the summary in-place
with the full session.
"""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path

from PyQt6.QtCore import Qt, QThread, pyqtSignal
from PyQt6.QtWidgets import (
    QFileDialog,
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
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from core.http_client import DisplayClient, Endpoint, probe
from core.sd_scan import CardKind, detect, scan_sessions
from core.session import Session, list_local_sessions, load_session_file
from core.video_dl import FileDownloadWorker, cache_dir
from ui.widgets.download_dialog import DownloadDialog


# ── Worker threads (unchanged from earlier rev) ────────────────────────────


class _ListSummariesWorker(QThread):
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
                self.finished_err.emit(
                    f"Display unter {self.host}:{self.port} nicht erreichbar."
                )
                return
            self.finished_ok.emit(DisplayClient(ep).list_sessions())
        except Exception as e:  # noqa: BLE001
            self.finished_err.emit(str(e))


# ── Item-data roles in the tree: distinguish track-row from session-row ────

_ROLE_KIND = Qt.ItemDataRole.UserRole + 1
_ROLE_REF  = Qt.ItemDataRole.UserRole + 2
_KIND_TRACK   = "track"
_KIND_SESSION = "session"


class SessionsView(QWidget):
    # Emits a list of (session, lap) pairs to the AnalyseView via MainWindow.
    analyse_requested = pyqtSignal(object)   # list[tuple[Session, Lap]]

    def __init__(self, main_window) -> None:  # noqa: ANN001
        super().__init__()
        self._mw = main_window
        self._sessions: list[Session] = []
        self._http_endpoint: Endpoint | None = None
        self._list_worker: _ListSummariesWorker | None = None
        self._basket: list[tuple[Session, int]] = []   # (session, lap_index)

        # ── Top action bar ─────────────────────────────────────────────
        self.btn_open_folder = QPushButton("📁 Lokaler Ordner…")
        self.btn_open_folder.clicked.connect(self._open_folder)
        self.btn_open_sd = QPushButton("💾 SD-Karte…")
        self.btn_open_sd.clicked.connect(self._open_sd_card)
        self.host_input = QLineEdit("192.168.4.1")
        self.host_input.setMaximumWidth(180)
        self.btn_fetch = QPushButton("📡 Vom Display laden")
        self.btn_fetch.clicked.connect(self._fetch_http)

        top = QHBoxLayout()
        top.addWidget(self.btn_open_folder)
        top.addWidget(self.btn_open_sd)
        top.addSpacing(20)
        top.addWidget(QLabel("Display:"))
        top.addWidget(self.host_input)
        top.addWidget(self.btn_fetch)
        top.addStretch(1)

        # ── Column 1: Track tree ───────────────────────────────────────
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Strecke / Sessions", "Sessions", "Runden"])
        self.tree.header().setSectionResizeMode(
            0, QHeaderView.ResizeMode.Stretch
        )
        self.tree.header().setSectionResizeMode(
            1, QHeaderView.ResizeMode.ResizeToContents
        )
        self.tree.header().setSectionResizeMode(
            2, QHeaderView.ResizeMode.ResizeToContents
        )
        self.tree.itemSelectionChanged.connect(self._on_tree_selection)
        self.tree.itemDoubleClicked.connect(self._on_tree_double_click)

        col1 = QVBoxLayout()
        col1.addWidget(QLabel("Strecken & Sessions"))
        col1.addWidget(self.tree, stretch=1)

        self.btn_download = QPushButton("⬇  Session downloaden")
        self.btn_download.setEnabled(False)
        self.btn_download.clicked.connect(self._download_selected_session)
        col1.addWidget(self.btn_download)

        col1_w = QWidget(); col1_w.setLayout(col1)

        # ── Column 2: Laps of currently selected session ───────────────
        self.lap_table = QTableWidget(0, 5)
        self.lap_table.setHorizontalHeaderLabels(
            ["Lap", "Zeit", "S1", "S2", "S3"]
        )
        self.lap_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self.lap_table.verticalHeader().setVisible(False)
        self.lap_table.setEditTriggers(
            QTableWidget.EditTrigger.NoEditTriggers
        )
        self.lap_table.setSelectionMode(
            QTableWidget.SelectionMode.ExtendedSelection
        )

        self.btn_add_basket = QPushButton("➕ Markierte Laps zur Auswahl")
        self.btn_add_basket.setEnabled(False)
        self.btn_add_basket.clicked.connect(self._add_to_basket)

        col2 = QVBoxLayout()
        self.lbl_session_title = QLabel("Wähle eine Session links")
        col2.addWidget(self.lbl_session_title)
        col2.addWidget(self.lap_table, stretch=1)
        col2.addWidget(self.btn_add_basket)
        col2_w = QWidget(); col2_w.setLayout(col2)

        # ── Column 3: Cross-session compare basket ─────────────────────
        self.basket_list = QListWidget()
        self.basket_list.setSelectionMode(
            QListWidget.SelectionMode.ExtendedSelection
        )

        self.btn_remove_basket = QPushButton("✕ Aus Auswahl entfernen")
        self.btn_remove_basket.clicked.connect(self._remove_from_basket)
        self.btn_clear_basket = QPushButton("Leeren")
        self.btn_clear_basket.clicked.connect(self._clear_basket)
        self.btn_analyse = QPushButton("📈 Analysieren →")
        self.btn_analyse.setEnabled(False)
        self.btn_analyse.clicked.connect(self._send_basket_to_analyse)

        col3 = QVBoxLayout()
        col3.addWidget(QLabel("Vergleich-Auswahl"))
        col3.addWidget(self.basket_list, stretch=1)
        basket_btns = QHBoxLayout()
        basket_btns.addWidget(self.btn_remove_basket)
        basket_btns.addWidget(self.btn_clear_basket)
        col3.addLayout(basket_btns)
        col3.addWidget(self.btn_analyse)
        col3_w = QWidget(); col3_w.setLayout(col3)

        # ── Compose ────────────────────────────────────────────────────
        split = QSplitter(Qt.Orientation.Horizontal)
        split.addWidget(col1_w)
        split.addWidget(col2_w)
        split.addWidget(col3_w)
        split.setStretchFactor(0, 3)
        split.setStretchFactor(1, 3)
        split.setStretchFactor(2, 2)

        root = QVBoxLayout(self)
        root.addLayout(top)
        root.addWidget(split, stretch=1)

    # ── Loading sessions (sources) ─────────────────────────────────────

    def _open_folder(self) -> None:
        folder = QFileDialog.getExistingDirectory(
            self, "Sessions-Ordner wählen (sd_card/sessions)"
        )
        if not folder:
            return
        sessions = list_local_sessions(Path(folder))
        if not sessions:
            QMessageBox.information(
                self, "Keine Sessions", "Im Ordner keine *.json gefunden."
            )
            return
        self._http_endpoint = None
        self._set_sessions(sessions)
        self._mw.set_status(f"{len(sessions)} Session(s) aus {folder}")

    def _open_sd_card(self) -> None:
        folder = QFileDialog.getExistingDirectory(
            self, "SD-Karte / Laufwerk wählen"
        )
        if not folder:
            return
        root = Path(folder)
        if detect(root) != CardKind.LAPTIMER:
            QMessageBox.information(
                self, "SD-Karte",
                "Keine Laptimer-SD-Struktur erkannt (sessions/-Ordner fehlt).",
            )
            return
        sessions: list[Session] = []
        for p in scan_sessions(root):
            try:
                sessions.append(load_session_file(p))
            except (OSError, ValueError):
                continue
        self._http_endpoint = None
        self._set_sessions(sessions)
        self._mw.set_status(f"SD: {len(sessions)} Session(s)")

    def _fetch_http(self) -> None:
        host = self.host_input.text().strip()
        if not host:
            return
        self.btn_fetch.setEnabled(False)
        self._mw.set_status(f"Lade Sessions von {host}…")
        self._list_worker = _ListSummariesWorker(host, 80)
        self._list_worker.finished_ok.connect(self._on_list_ok)
        self._list_worker.finished_err.connect(self._on_list_err)
        self._list_worker.finished.connect(
            lambda: self.btn_fetch.setEnabled(True)
        )
        self._list_worker.start()

    def _on_list_ok(self, sessions: list) -> None:
        host = self.host_input.text().strip()
        self._http_endpoint = Endpoint(host=host, port=80)
        self._set_sessions(sessions)
        self._mw.set_connection(
            f"Display: {host} ({len(sessions)} Sessions)"
        )
        self._mw.set_status(
            f"{len(sessions)} Session(s) vom Display geladen — "
            "zum Analysieren erst herunterladen."
        )

    def _on_list_err(self, msg: str) -> None:
        QMessageBox.warning(self, "Verbindungsfehler", msg)
        self._mw.set_status("Fehler beim Laden")

    # ── Tree population ────────────────────────────────────────────────

    def _set_sessions(self, sessions: list[Session]) -> None:
        self._sessions = sessions
        self._rebuild_tree()
        self.lap_table.setRowCount(0)
        self.lbl_session_title.setText("Wähle eine Session links")
        self.btn_add_basket.setEnabled(False)

    def _rebuild_tree(self) -> None:
        # Group sessions by track, preserving insertion order.
        by_track: dict[str, list[Session]] = defaultdict(list)
        for s in self._sessions:
            by_track[s.track or "(ohne Strecke)"].append(s)

        self.tree.blockSignals(True)
        self.tree.clear()
        for track, group in sorted(by_track.items(), key=lambda kv: kv[0].lower()):
            laps_total = sum(s.lap_count for s in group)
            track_item = QTreeWidgetItem(
                [track, str(len(group)), str(laps_total)]
            )
            track_item.setData(0, _ROLE_KIND, _KIND_TRACK)
            track_item.setData(0, _ROLE_REF, track)
            track_item.setFirstColumnSpanned(False)

            # Sort sessions by best lap time (ascending; 0 = no time → last)
            for s in sorted(group,
                            key=lambda x: (x.best_ms or 10**12,
                                           x.name or x.id)):
                bl = s.best_lap_str()
                tag = " · 💾" if not s.is_summary else " · summary"
                child = QTreeWidgetItem(
                    [f"{s.name or s.id}{tag}", "", str(s.lap_count)]
                )
                # Show best-lap right under the session name in col 0
                child.setToolTip(0, f"Best Lap: {bl}")
                child.setData(0, _ROLE_KIND, _KIND_SESSION)
                child.setData(0, _ROLE_REF, s.id)
                track_item.addChild(child)
            self.tree.addTopLevelItem(track_item)
            track_item.setExpanded(True)
        self.tree.blockSignals(False)

    def _find_session(self, sess_id: str) -> Session | None:
        for s in self._sessions:
            if s.id == sess_id:
                return s
        return None

    def _selected_session(self) -> Session | None:
        items = self.tree.selectedItems()
        if not items:
            return None
        it = items[0]
        if it.data(0, _ROLE_KIND) != _KIND_SESSION:
            return None
        return self._find_session(str(it.data(0, _ROLE_REF)))

    # ── Tree selection → laps panel ────────────────────────────────────

    def _on_tree_selection(self) -> None:
        s = self._selected_session()
        if s is None:
            self.lap_table.setRowCount(0)
            self.lbl_session_title.setText("Wähle eine Session links")
            self.btn_download.setEnabled(False)
            self.btn_add_basket.setEnabled(False)
            return
        self.lbl_session_title.setText(
            f"{s.track} — {s.name or s.id} · {s.lap_count} Runden"
        )
        # Download is meaningful only for HTTP-sourced summary sessions.
        self.btn_download.setEnabled(
            s.is_summary and self._http_endpoint is not None
        )
        if s.is_summary:
            self.lap_table.setRowCount(1)
            ph = QTableWidgetItem(
                "Erst Session downloaden (oben links) — Laps sind noch nicht da."
            )
            self.lap_table.setSpan(0, 0, 1, 5)
            self.lap_table.setItem(0, 0, ph)
            self.btn_add_basket.setEnabled(False)
            return
        self._render_laps(s)

    def _on_tree_double_click(self, item: QTreeWidgetItem, _col: int) -> None:
        if item.data(0, _ROLE_KIND) == _KIND_SESSION:
            s = self._selected_session()
            if s and s.is_summary and self._http_endpoint is not None:
                self._download_selected_session()

    def _render_laps(self, s: Session) -> None:
        # Clear any leftover row span from the placeholder state
        self.lap_table.clearSpans()
        self.lap_table.setRowCount(len(s.laps))
        for i, lap in enumerate(s.laps):
            self.lap_table.setItem(i, 0, QTableWidgetItem(str(lap.number)))
            self.lap_table.setItem(i, 1, QTableWidgetItem(lap.lap_time_str()))
            for col, idx in ((2, 0), (3, 1), (4, 2)):
                ms = lap.sectors[idx] if idx < len(lap.sectors) else 0
                self.lap_table.setItem(
                    i, col,
                    QTableWidgetItem(f"{ms / 1000.0:.3f}" if ms else "—"),
                )
        self.btn_add_basket.setEnabled(True)

    # ── Download (HTTP summary → full) ─────────────────────────────────

    def _download_selected_session(self) -> None:
        s = self._selected_session()
        if s is None or not s.is_summary or self._http_endpoint is None:
            return
        url = self._http_endpoint.url(f"/session/{s.id}")
        dest = cache_dir("sessions") / f"{s.id}.json"
        worker = FileDownloadWorker(url, dest)
        dlg = DownloadDialog(self, f"Session {s.id}")
        dlg.attach(worker)
        worker.start()
        dlg.exec()
        if not dlg.result_path():
            return
        try:
            full = load_session_file(Path(dlg.result_path()))
        except (OSError, ValueError) as e:
            QMessageBox.warning(self, "Fehler", str(e)); return
        # Replace summary in-place + refresh tree (preserve selection by id).
        for i, x in enumerate(self._sessions):
            if x.id == s.id:
                self._sessions[i] = full
                break
        self._rebuild_tree()
        self._reselect_session(full.id)
        self._mw.set_status(f"Heruntergeladen: {full.lap_count} Runden")

    def _reselect_session(self, sess_id: str) -> None:
        for i in range(self.tree.topLevelItemCount()):
            top = self.tree.topLevelItem(i)
            for j in range(top.childCount()):
                ch = top.child(j)
                if (ch.data(0, _ROLE_KIND) == _KIND_SESSION
                        and ch.data(0, _ROLE_REF) == sess_id):
                    self.tree.setCurrentItem(ch)
                    return

    # ── Compare basket ─────────────────────────────────────────────────

    def _add_to_basket(self) -> None:
        s = self._selected_session()
        if s is None or s.is_summary:
            return
        rows = sorted({i.row() for i in self.lap_table.selectedItems()})
        if not rows:
            QMessageBox.information(
                self, "Auswahl", "Markiere zuerst Runden in der Mitte."
            )
            return
        added = 0
        for row in rows:
            if row >= len(s.laps):
                continue
            key = (s.id, row)
            # Skip duplicates
            if any(b[0].id == s.id and b[1] == row for b in self._basket):
                continue
            self._basket.append((s, row))
            added += 1
        self._refresh_basket()
        self._mw.set_status(f"{added} Lap(s) zur Auswahl hinzugefügt")

    def _refresh_basket(self) -> None:
        self.basket_list.clear()
        for s, row in self._basket:
            lap = s.laps[row]
            label = (f"{s.track} — {s.name or s.id} · "
                     f"Lap {lap.number} · {lap.lap_time_str()}")
            self.basket_list.addItem(QListWidgetItem(label))
        self.btn_analyse.setEnabled(bool(self._basket))

    def _remove_from_basket(self) -> None:
        rows = sorted({i.row() for i in self.basket_list.selectedItems()},
                      reverse=True)
        for r in rows:
            if 0 <= r < len(self._basket):
                del self._basket[r]
        self._refresh_basket()

    def _clear_basket(self) -> None:
        self._basket.clear()
        self._refresh_basket()

    def _send_basket_to_analyse(self) -> None:
        if not self._basket:
            return
        # Materialise to (session, lap)-pairs for AnalyseView
        out = [(s, s.laps[row]) for s, row in self._basket
               if 0 <= row < len(s.laps)]
        if not out:
            return
        self.analyse_requested.emit(out)
