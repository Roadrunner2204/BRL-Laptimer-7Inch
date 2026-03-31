#!/usr/bin/env python3
"""
Logo JPG → C-Array Konvertierer
================================
Verwendung:
    python tools/jpg_to_c_array.py pfad/zu/logo.jpg

Ergebnis:
    src/assets/logo_data.h   — Array-Deklaration
    src/assets/logo_data.cpp — Array-Daten (JPEG-Bytes)

Danach in src/assets/logo_placeholder.h die Zeile
    #define LOGO_USE_PLACEHOLDER 1
auskommentieren — der Splash-Screen lädt dann automatisch das echte Logo.
"""

import sys
import os
from pathlib import Path

def convert(src_path: str):
    src = Path(src_path)
    if not src.exists():
        print(f"Fehler: Datei nicht gefunden: {src}")
        sys.exit(1)

    data = src.read_bytes()
    size = len(data)
    print(f"Logo: {src.name}  ({size} Bytes)")

    # Output paths
    out_dir = Path(__file__).parent.parent / "src" / "assets"
    out_dir.mkdir(parents=True, exist_ok=True)
    h_path   = out_dir / "logo_data.h"
    cpp_path = out_dir / "logo_data.cpp"

    # Get image dimensions (simple JPEG SOF0/SOF2 parser)
    w, h = 0, 0
    i = 0
    while i < len(data) - 4:
        if data[i] == 0xFF and data[i+1] in (0xC0, 0xC2):
            h = (data[i+5] << 8) | data[i+6]
            w = (data[i+7] << 8) | data[i+8]
            break
        i += 1
    print(f"Bildgröße: {w} x {h} px")

    # ---- .h ----
    h_content = f"""#pragma once
#include <stdint.h>

// Auto-generiert durch tools/jpg_to_c_array.py
// Quelle: {src.name}  ({w}x{h} px, {size} Bytes)

extern const uint8_t  LOGO_JPG[];
extern const uint32_t LOGO_JPG_SIZE;
#define LOGO_JPG_WIDTH  {w}
#define LOGO_JPG_HEIGHT {h}
"""
    h_path.write_text(h_content)
    print(f"Geschrieben: {h_path}")

    # ---- .cpp ----
    hex_vals = ", ".join(f"0x{b:02X}" for b in data)
    cpp_content = f"""// Auto-generiert durch tools/jpg_to_c_array.py
// Quelle: {src.name}  ({w}x{h} px, {size} Bytes)
#include "logo_data.h"

const uint32_t LOGO_JPG_SIZE = {size}U;

const uint8_t LOGO_JPG[] = {{
{hex_vals}
}};
"""
    cpp_path.write_text(cpp_content)
    print(f"Geschrieben: {cpp_path}")

    print()
    print("Nächster Schritt:")
    print("  In src/assets/logo_placeholder.h die Zeile")
    print("  '#define LOGO_USE_PLACEHOLDER 1' auskommentieren.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    convert(sys.argv[1])
