# -*- mode: python ; coding: utf-8 -*-
#
# PyInstaller spec for BRL Studio — onefile build
#
# Result: dist/BRL-Studio.exe
#
# Local build:
#   cd tools/brl_studio
#   pip install -r requirements.txt
#   pip install pyinstaller
#   pyinstaller ../brl_studio.spec
#
# CI: extend .github/workflows/build-installers.yml with a Studio job.

from pathlib import Path

PROJECT_DIR = Path(SPECPATH) / 'brl_studio'
ENTRY = str(PROJECT_DIR / 'brl_studio.py')

# Bundle the assets folder (icons, future Leaflet HTML, etc.) so MEIPASS
# resolves it the same way at runtime.
datas = []
assets_dir = PROJECT_DIR / 'assets'
if assets_dir.exists():
    datas.append((str(assets_dir), 'assets'))

a = Analysis(
    [ENTRY],
    pathex=[str(PROJECT_DIR)],
    binaries=[],
    datas=datas,
    hiddenimports=[
        # PyQt6 sub-modules that PyInstaller's static analysis sometimes
        # misses when imported via the meta paths used by Qt plugins.
        'PyQt6.QtCore',
        'PyQt6.QtGui',
        'PyQt6.QtWidgets',
        # Phase 1+: WebEngine for Leaflet maps. Listed early so the spec
        # file doesn't need editing again when the view goes live.
        'PyQt6.QtWebEngineCore',
        'PyQt6.QtWebEngineWidgets',
        # Phase 1+: pyqtgraph
        'pyqtgraph',
        # Phase 2+: libVLC binding
        'vlc',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'pytest',
        'tkinter',          # we use Qt — keep tk out of the bundle
        'matplotlib',
        'numpy.distutils',
    ],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='BRL-Studio',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,                  # GUI app — no console window
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=str(PROJECT_DIR / 'assets' / 'icon.ico')
        if (PROJECT_DIR / 'assets' / 'icon.ico').exists() else None,
)
