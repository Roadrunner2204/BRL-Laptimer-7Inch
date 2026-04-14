# -*- mode: python ; coding: utf-8 -*-
#
# PyInstaller Spec für BRL Fahrzeugprofil-Tool
#
# Bauen (vom Repo-Root):
#   pip install pyinstaller pycryptodome
#   pyinstaller tools/brl_format_gui.spec
#
# Ausgabe: dist/BRL-Format-Tool/BRL-Format-Tool.exe

from pathlib import Path

# SPECPATH = Verzeichnis der Spec-Datei (tools/)
src = str(Path(SPECPATH) / 'brl_format_gui.py')

a = Analysis(
    [src],
    pathex=[SPECPATH],
    binaries=[],
    datas=[],
    hiddenimports=[
        'Crypto',
        'Crypto.Cipher',
        'Crypto.Cipher.AES',
        'Crypto.Cipher._mode_cbc',
        'Crypto.Cipher._mode_ecb',
        'Crypto.Util',
        'Crypto.Util.Padding',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['pytest', 'numpy', 'pandas', 'matplotlib', 'openpyxl'],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='BRL-Format-Tool',
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
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='BRL-Format-Tool',
)
