# -*- mode: python ; coding: utf-8 -*-
#
# PyInstaller Spec für TRX → Excel Konverter
#
# Bauen (vom Repo-Root):
#   pip install pyinstaller pycryptodome openpyxl
#   pyinstaller tools/trx_to_excel_gui.spec
#
# Ausgabe: dist/TRX-Excel-Konverter/TRX-Excel-Konverter.exe

from pathlib import Path

# SPECPATH = Verzeichnis der Spec-Datei (tools/)
src = str(Path(SPECPATH) / 'trx_to_excel_gui.py')

a = Analysis(
    [src],
    pathex=[SPECPATH],
    binaries=[],
    datas=[],
    hiddenimports=[
        'Crypto',
        'Crypto.Cipher',
        'Crypto.Cipher.AES',
        'Crypto.Cipher._mode_ecb',
        'Crypto.Util',
        'Crypto.Util.Padding',
        'openpyxl',
        'openpyxl.styles',
        'openpyxl.styles.fills',
        'openpyxl.styles.fonts',
        'openpyxl.styles.borders',
        'openpyxl.styles.alignment',
        'openpyxl.utils',
        'openpyxl.utils.cell',
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
    name='TRX-Excel-Konverter',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,          # kein schwarzes Konsolenfenster
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    # icon='installer/icon.ico',  # optional: Pfad zu .ico Datei
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='TRX-Excel-Konverter',
)
