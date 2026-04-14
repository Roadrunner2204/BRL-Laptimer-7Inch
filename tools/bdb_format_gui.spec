# -*- mode: python ; coding: utf-8 -*-
#
# PyInstaller Spec für BDB → BRL Track-Konverter (GUI)
#
# Bauen (vom Repo-Root):
#   pip install pyinstaller pycryptodome
#   pyinstaller tools/bdb_format_gui.spec
#
# Ausgabe: dist/BDB-Track-Konverter/BDB-Track-Konverter.exe

from pathlib import Path

src_dir = Path(SPECPATH)
src     = str(src_dir / "bdb_format_gui.py")

# bdb_format.py wird zur Laufzeit importiert — als Daten-Modul mitbundeln,
# damit PyInstaller es sicher mitnimmt.
datas = [
    (str(src_dir / "bdb_format.py"), "."),
]

a = Analysis(
    [src],
    pathex=[str(src_dir)],
    binaries=[],
    datas=datas,
    hiddenimports=[
        "bdb_format",
        "tkinter",
        "tkinter.filedialog",
        "tkinter.messagebox",
        "Crypto",
        "Crypto.Cipher",
        "Crypto.Cipher.AES",
        "Crypto.Cipher._mode_cbc",
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["pytest", "numpy", "pandas", "matplotlib", "openpyxl"],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="BDB-Track-Konverter",
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
    # icon="installer/icon.ico",  # optional
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name="BDB-Track-Konverter",
)
