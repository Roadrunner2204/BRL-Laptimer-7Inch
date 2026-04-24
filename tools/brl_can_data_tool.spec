# -*- mode: python ; coding: utf-8 -*-
#
# PyInstaller Spec fuer BRL Can Data Tool — ONEFILE Build
#
# Ergebnis: dist/BRL-Can-Data-Tool.exe  (eine einzelne portable Datei)
#
# Lokaler Build:
#   pip install pyinstaller pycryptodome openpyxl
#   pyinstaller tools/brl_can_data_tool.spec
#
# In GitHub Actions laeuft derselbe Aufruf (.github/workflows/build-installers.yml).

from pathlib import Path

src = str(Path(SPECPATH) / 'brl_can_data_tool.py')

# logo.png mitbuendeln, falls vorhanden (Tool sucht es via sys._MEIPASS)
datas = []
logo = Path(SPECPATH) / 'logo.png'
if logo.exists():
    datas.append((str(logo), '.'))

a = Analysis(
    [src],
    pathex=[SPECPATH],
    binaries=[],
    datas=datas,
    hiddenimports=[
        'Crypto',
        'Crypto.Cipher',
        'Crypto.Cipher.AES',
        'Crypto.Cipher._mode_cbc',
        'Crypto.Cipher._mode_ecb',
        'Crypto.Util',
        'Crypto.Util.Padding',
        'openpyxl',
        'openpyxl.styles',
        'openpyxl.utils',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['pytest', 'numpy', 'pandas', 'matplotlib'],
    noarchive=False,
)

pyz = PYZ(a.pure)

# ── Onefile ──────────────────────────────────────────────────────────────────
# Kein COLLECT-Block: a.binaries + a.datas werden direkt in die .exe gepackt.
exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='BRL-Can-Data-Tool',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,                # kein Konsolenfenster beim Start
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
