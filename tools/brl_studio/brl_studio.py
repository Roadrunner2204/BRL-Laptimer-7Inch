#!/usr/bin/env python3
"""
BRL Studio — Telemetry analysis & configuration suite

Companion desktop app for the BRL Laptimer (ESP32-P4) and the external
camera module. Loads sessions from local .brl files, the laptimer's WiFi
HTTP API, or the camera module's SD card; analyses laps with multi-channel
charts; configures track sectors and dashboard layouts; uploads them back
to the laptimer.

Phase 0 (current):
    - App skeleton (sidebar navigation + view stack)
    - Sessions browser (local folder + Display HTTP /sessions)
    - Build pipeline (PyInstaller .spec + Inno Setup)

Later phases see brl_studio/README.md.

Run from source:
    cd tools/brl_studio
    pip install -r requirements.txt
    python brl_studio.py

Build single-exe installer (Windows):
    pyinstaller ../brl_studio.spec
    "C:\\Program Files (x86)\\Inno Setup 6\\ISCC.exe" \\
        /DAppVersion=0.1.0 ../../installer/brl_studio_setup.iss
"""

from __future__ import annotations

import sys
from pathlib import Path

# When frozen by PyInstaller the package layout is flattened; make the
# app's package importable in both source and bundled forms.
HERE = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from PyQt6.QtCore import QSettings
from PyQt6.QtWidgets import QApplication

from core.http_client import Endpoint
from core.obd_status import get_obd_status_poller
from core.theme import apply_theme, get_active_theme
from core.tile_server import start_tile_server, stop_tile_server
from ui.main_window import MainWindow

APP_VERSION = "0.1.0"
APP_TITLE = "BRL Studio"


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName(APP_TITLE)
    app.setApplicationVersion(APP_VERSION)
    app.setOrganizationName("Bavarian RaceLabs")

    # Apply persisted theme before any widget is constructed so the QSS
    # propagates into every dialog.
    apply_theme(app, get_active_theme())

    # Boot the tile + Leaflet asset proxy. The map widget reads its port
    # from server_url() when constructing the Leaflet HTML, so the server
    # must be live before any view that hosts a map opens.
    start_tile_server()
    app.aboutToQuit.connect(stop_tile_server)

    # Kick off the OBD-status poller against the persisted Display host.
    # The poller is idempotent — ConfigView re-configures it whenever the
    # user changes the host. is_live() returns False until the first
    # successful fetch, so the slot pickers degrade gracefully.
    s = QSettings()
    host = str(s.value("display/host", "192.168.4.1", str))
    port = int(s.value("display/port", 80, int))
    poller = get_obd_status_poller()
    poller.configure(Endpoint(host=host, port=port))
    app.aboutToQuit.connect(poller.stop)

    win = MainWindow(version=APP_VERSION)
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
