"""
Main window: sidebar nav + view stack.

Each sidebar entry maps to one QWidget in the stack. View widgets are
self-contained and own their data models, so switching tabs is cheap and
the application can grow phase-by-phase without a central god-object.
"""

from __future__ import annotations

from PyQt6.QtCore import Qt, QSize
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QStackedWidget,
    QStatusBar,
    QVBoxLayout,
    QWidget,
)

from ui.analyse_view import AnalyseView
from ui.config_view import ConfigView
from ui.layout_view import LayoutView
from ui.sessions_view import SessionsView
from ui.tracks_view import TracksView
from ui.video_export_view import VideoExportView

# Each tuple: (icon-emoji-or-text, label, factory). Factories are deferred
# so heavy imports (QtWebEngine, vlc) don't load until the view is opened.
NAV_ENTRIES = [
    ("🏁", "Sessions",     SessionsView),
    ("📈", "Analyse",      AnalyseView),
    ("🎬", "Video Export", VideoExportView),
    ("🛣️", "Tracks",       TracksView),
    ("🎛️", "Layouts",      LayoutView),
    ("⚙️",  "Verbindung",  ConfigView),
]


class MainWindow(QMainWindow):
    def __init__(self, version: str) -> None:
        super().__init__()
        self.setWindowTitle(f"BRL Studio {version}")
        self.resize(1400, 900)

        central = QWidget(self)
        root = QHBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # ── Sidebar ─────────────────────────────────────────────────────
        self.nav = QListWidget()
        self.nav.setObjectName("NavList")  # picked up by the theme stylesheet
        self.nav.setFixedWidth(200)
        self.nav.setIconSize(QSize(20, 20))
        self.nav.setSpacing(2)
        f = QFont()
        f.setPointSize(11)
        self.nav.setFont(f)

        # ── View stack ──────────────────────────────────────────────────
        self.stack = QStackedWidget()

        self._views: dict[str, QWidget] = {}
        for icon, label, factory in NAV_ENTRIES:
            self.nav.addItem(QListWidgetItem(f"  {icon}   {label}"))
            view = factory(self)
            self.stack.addWidget(view)
            self._views[label] = view

        self.nav.currentRowChanged.connect(self.stack.setCurrentIndex)
        self.nav.setCurrentRow(0)

        # Sessions → Analyse routing: SessionsView emits analyse_requested
        # when the user picks a session; we forward into AnalyseView and
        # switch the active tab.
        sessions = self._views.get("Sessions")
        analyse = self._views.get("Analyse")
        if sessions is not None and analyse is not None:
            sessions.analyse_requested.connect(self._open_for_analysis)

        root.addWidget(self.nav)
        root.addWidget(self.stack, stretch=1)
        self.setCentralWidget(central)

        # ── Status bar ──────────────────────────────────────────────────
        sb = QStatusBar(self)
        self.lbl_status = QLabel("Bereit")
        sb.addWidget(self.lbl_status, 1)
        self.lbl_conn = QLabel("Display: nicht verbunden")
        sb.addPermanentWidget(self.lbl_conn)
        self.setStatusBar(sb)

    def set_status(self, msg: str) -> None:
        self.lbl_status.setText(msg)

    def set_connection(self, msg: str) -> None:
        self.lbl_conn.setText(msg)

    def _open_for_analysis(self, payload) -> None:  # noqa: ANN001
        """payload is now a list[(Session, Lap)] from the basket."""
        analyse = self._views.get("Analyse")
        if analyse is None:
            return
        if not isinstance(payload, list) or not payload:
            return
        analyse.set_laps(payload)
        labels = [lbl for _, lbl, _ in NAV_ENTRIES]
        if "Analyse" in labels:
            self.nav.setCurrentRow(labels.index("Analyse"))
