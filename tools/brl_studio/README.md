# BRL Studio

Telemetry analysis and configuration suite for the BRL Laptimer (ESP32-P4)
and the external camera module. Companion desktop app to the on-display UI
and the Android app — same data, more horsepower.

## Status

All seven phases are functional in the desktop app. Two firmware-side
endpoints (`POST /dash_config`, `POST /lvt`) are still TODO on the
laptimer; until they exist the **Layouts** tab saves locally and the
upload button surfaces a friendly 404 message. A Display-side `.lvt`
*renderer* is also a follow-up firmware task — Studio writes the format,
the Display will need new code in `main/ui/` to render it.

## Roadmap

| Phase | Status | Scope |
|-------|--------|-------|
| 0 | ✅ | Skeleton + Sessions browser + build pipeline |
| 1 | ✅ | Multi-channel charts (Speed / G-lat / G-long / Delta), map sync, sector bars, scrubbable cursor |
| 2 | ✅ | Video playback (AVI/MJPEG via libVLC) with HUD overlay (Speed / Lap-Zeit / Pedale / G-Meter) |
| 3 | ✅ | SD-card browser (laptimer + cam layouts) + WiFi download from Display + Cam (with progress dialog) |
| 4 | ✅ | Track editor — Leaflet/OSM map with draggable S/F + sector markers, click-to-place picking, `POST /track` to Display |
| 5 | ✅ | Display layout configurator — Z1/Z2/Z3 slot pickers + global settings + 1024×600 live preview |
| 6 | ✅ | Drag-and-drop custom layout designer (`.lvt`). Display-side renderer is a separate firmware task. |
| 7 | ✅ | CSV export of derived channels per lap; MP4 export with embedded HUD overlay via ffmpeg |

## Pending firmware work

These are nice-to-haves on the laptimer side; nothing blocks Studio from
being useful today:

- `POST /dash_config` — accept and persist the JSON Studio sends from the
  Layouts tab (Slot mode).
- `POST /lvt` — accept `.lvt` files and store them on SD; renderer in
  `main/ui/` to draw them instead of the hard-coded zones.
- Persist OBD/CAN/analog channels per track point in the session JSON
  written by `session_store.cpp` so the Analyse view can chart them
  alongside GPS-derived channels.

## Running from source

```powershell
cd tools\brl_studio
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python brl_studio.py
```

Python 3.10+ recommended (uses PEP 604 type hints).

## Building the Windows installer

Same pipeline as the BRL Can Data Tool — PyInstaller produces a single
`.exe`, Inno Setup wraps it in a setup wizard.

```powershell
# 1) PyInstaller — produces dist/BRL-Studio.exe
cd tools\brl_studio
pip install pyinstaller
pyinstaller ..\brl_studio.spec

# 2) Inno Setup — produces dist/BRL-Studio-Setup-v0.1.0.exe
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" `
    /DAppVersion=0.1.0 ..\..\installer\brl_studio_setup.iss
```

The CI workflow in `.github/workflows/` can be extended to build Studio
alongside the existing tools.

## Architecture

```
brl_studio/
├── brl_studio.py          # entry point (QApplication + MainWindow)
├── core/                  # domain layer — minimal Qt usage
│   ├── session.py         # JSON session parser + dataclasses
│   ├── analysis.py        # GPS-derived channels (speed, G, delta)
│   ├── telemetry.py       # NDJSON sidecar parser (gps/obd/ana/lap)
│   ├── http_client.py     # Display + Cam REST API
│   ├── video_dl.py        # streaming download worker + cache dir
│   ├── sd_scan.py         # mounted SD card detection + file lists
│   ├── dash_config.py     # FieldId enum + DashConfig schema
│   ├── lvt_format.py      # .lvt schema + widget defaults
│   └── export.py          # CSV + ffmpeg MP4 (with HUD burn-in)
├── ui/                    # Qt layer — every view is self-contained
│   ├── main_window.py     # sidebar + view stack + sess→analyse routing
│   ├── sessions_view.py   # local + HTTP + SD + download
│   ├── analyse_view.py    # multi-channel charts + map + sector bars
│   ├── video_view.py      # libVLC + HUD + cam/SD/local + MP4 export
│   ├── tracks_view.py     # track editor (Leaflet picker + drag)
│   ├── layout_view.py     # Slot-Editor + Custom-Designer (toggle)
│   ├── config_view.py     # connection settings (QSettings)
│   └── widgets/
│       ├── chart.py            # pyqtgraph multi-trace
│       ├── map.py              # Leaflet + QWebChannel bridge
│       ├── sector_bars.py      # custom-painted bar comparison
│       ├── video_player.py     # libVLC wrapper (HWND/XWindow)
│       ├── hud_overlay.py      # transparent HUD QWidget
│       ├── download_dialog.py  # progress dialog for FileDownloadWorker
│       ├── lvt_designer.py     # QGraphicsView 1024×600 canvas
│       └── lvt_properties.py   # type-aware property panel
└── assets/                # icons (added per phase)
```

The `core/` layer keeps Qt usage to QThread/QStandardPaths/QImage — that
lets the export pipeline run without a visible window and keeps the
domain logic close to library code.

## Data sources

Phase 0 reads sessions from:

1. **Local folder** — point at any directory with `*.json` files in the
   format written by `main/storage/session_store.cpp` (or an extracted SD
   card's `/sessions` folder).
2. **Display HTTP** — connect to the laptimer's WiFi (default
   `192.168.4.1`, port 80) and `GET /sessions`.

Phase 3 will add the camera module as a third source for AVIs +
NDJSON sidecars (`/videos/list` and `/telemetry/<id>`).
