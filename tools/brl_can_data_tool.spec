# -*- mode: python ; coding: utf-8 -*-
#
# PyInstaller Spec fuer BRL Can Data Tool (vereintes TRX/TRI/BRL Werkzeug)
#
# Bauen (vom Repo-Root):
#   pip install pyinstaller pycryptodome openpyxl
#   pyinstaller tools/brl_can_data_tool.spec
#
# Ausgabe: dist/BRL-Can-Data-Tool/BRL-Can-Data-Tool.exe

from pathlib import Path

src = str(Path(SPECPATH) / 'brl_can_data_tool.py')

# logo.png mitnehmen falls vorhanden (Tool sucht es via _MEIPASS)
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

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='BRL-Can-Data-Tool',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='BRL-Can-Data-Tool',
)
