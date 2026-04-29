"""
Connection settings — Phase 0 functional.

Stores the Display IP and the Cam IP in QSettings so the SessionsView
and (later) VideoView can pick them up without re-entering. A reachability
test pings GET / on each.
"""

from __future__ import annotations

from PyQt6.QtCore import QSettings, Qt
from PyQt6.QtWidgets import (
    QApplication,
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from core.http_client import Endpoint, probe
from core.obd_status import get_obd_status_poller
from core.theme import THEME_DARK, THEME_LIGHT, apply_theme, get_active_theme


class ConfigView(QWidget):
    def __init__(self, main_window) -> None:  # noqa: ANN001
        super().__init__()
        self._mw = main_window
        self._s = QSettings()

        title = QLabel("⚙️   Verbindung")
        title.setStyleSheet("font-size: 22px; font-weight: 600;")

        # Display
        self.display_host = QLineEdit(self._s.value("display/host", "192.168.4.1", str))
        self.display_port = QSpinBox()
        self.display_port.setRange(1, 65535)
        self.display_port.setValue(int(self._s.value("display/port", 80, int)))

        btn_disp_test = QPushButton("Test")
        btn_disp_test.clicked.connect(
            lambda: self._test(self.display_host.text(),
                               self.display_port.value(), "Display")
        )

        # Cam
        self.cam_host = QLineEdit(self._s.value("cam/host", "192.168.4.2", str))
        self.cam_port = QSpinBox()
        self.cam_port.setRange(1, 65535)
        self.cam_port.setValue(int(self._s.value("cam/port", 80, int)))

        btn_cam_test = QPushButton("Test")
        btn_cam_test.clicked.connect(
            lambda: self._test(self.cam_host.text(),
                               self.cam_port.value(), "Kameramodul")
        )

        # Theme picker
        self.cb_theme = QComboBox()
        self.cb_theme.addItem("Dark",  THEME_DARK)
        self.cb_theme.addItem("Light", THEME_LIGHT)
        cur_idx = self.cb_theme.findData(get_active_theme())
        if cur_idx >= 0:
            self.cb_theme.setCurrentIndex(cur_idx)
        self.cb_theme.currentIndexChanged.connect(self._on_theme_changed)

        btn_save = QPushButton("Speichern")
        btn_save.clicked.connect(self._save)

        form = QFormLayout()
        form.addRow("Display Host", self._row(self.display_host, self.display_port, btn_disp_test))
        form.addRow("Cam Host",     self._row(self.cam_host,     self.cam_port,     btn_cam_test))
        form.addRow("Theme",        self.cb_theme)

        root = QVBoxLayout(self)
        root.setContentsMargins(40, 30, 40, 30)
        root.setAlignment(Qt.AlignmentFlag.AlignTop)
        root.addWidget(title)
        root.addSpacing(20)
        root.addLayout(form)
        root.addSpacing(20)
        root.addWidget(btn_save, alignment=Qt.AlignmentFlag.AlignLeft)

    @staticmethod
    def _row(host: QLineEdit, port: QSpinBox, btn: QPushButton) -> QHBoxLayout:
        h = QHBoxLayout()
        h.addWidget(host, 3)
        h.addWidget(QLabel(":"))
        h.addWidget(port, 1)
        h.addSpacing(8)
        h.addWidget(btn)
        return h

    def _test(self, host: str, port: int, label: str) -> None:
        ok = probe(Endpoint(host=host.strip(), port=port))
        if ok:
            QMessageBox.information(self, label, f"{label} erreichbar ✓")
        else:
            QMessageBox.warning(self, label, f"{label} nicht erreichbar.")

    def _save(self) -> None:
        host = self.display_host.text().strip()
        port = self.display_port.value()
        self._s.setValue("display/host", host)
        self._s.setValue("display/port", port)
        self._s.setValue("cam/host", self.cam_host.text().strip())
        self._s.setValue("cam/port", self.cam_port.value())
        # Re-target the OBD-status poller at the (possibly new) host so
        # the slot pickers in LayoutView/Properties pick up live fields.
        get_obd_status_poller().configure(Endpoint(host=host, port=port))
        self._mw.set_status("Einstellungen gespeichert")

    def _on_theme_changed(self, _idx: int) -> None:
        app = QApplication.instance()
        if app is None:
            return
        apply_theme(app, str(self.cb_theme.currentData()))
        self._mw.set_status(f"Theme: {self.cb_theme.currentText()}")
