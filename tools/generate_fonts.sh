#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# BRL Laptimer — LVGL Font Generator
#
# Generates Montserrat font C files with German/extended Latin characters
# (Ä Ö Ü ä ö ü ß and more) for use with LVGL 9.
#
# Requirements:
#   Node.js + npm:   https://nodejs.org
#   lv_font_conv:    npm install -g lv_font_conv
#   Font file:       place Montserrat-Regular.ttf in tools/
#                    (download from https://fonts.google.com/specimen/Montserrat)
#
# Usage:
#   cd tools && bash generate_fonts.sh
#
# Output:
#   src/ui/fonts/brl_font_montserrat_NN.c  (one per size)
#
# After running, set BRL_USE_EXTENDED_FONTS 1 in include/lv_conf.h
# ---------------------------------------------------------------------------

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FONT_FILE="${SCRIPT_DIR}/Montserrat-Regular.ttf"
OUTPUT_DIR="${SCRIPT_DIR}/../src/ui/fonts"

if [ ! -f "$FONT_FILE" ]; then
    echo "ERROR: $FONT_FILE not found."
    echo "       Download Montserrat-Regular.ttf from https://fonts.google.com/specimen/Montserrat"
    echo "       and place it in the tools/ directory."
    exit 1
fi

if ! command -v lv_font_conv &>/dev/null; then
    echo "ERROR: lv_font_conv not found."
    echo "       Run: npm install -g lv_font_conv"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Unicode ranges:
#   0x20-0x7E  Basic Latin (ASCII printable)
#   0xA0-0xFF  Latin-1 Supplement (includes Ä Ö Ü ä ö ü ß À Ã Ñ etc.)
RANGES="0x20-0x7E,0xA0-0xFF"

for SIZE in 14 16 20 24 32 40 48; do
    OUT="${OUTPUT_DIR}/brl_font_montserrat_${SIZE}.c"
    echo "  Generating size ${SIZE} → ${OUT}"
    lv_font_conv \
        --font "$FONT_FILE" \
        -r "$RANGES" \
        --size "$SIZE" \
        --format lvgl \
        --bpp 4 \
        --no-compress \
        --force-fast-kern-format \
        -o "$OUT"
done

echo ""
echo "Done! Generated fonts in src/ui/fonts/"
echo ""
echo "Next steps:"
echo "  1. Open include/lv_conf.h"
echo "  2. Change:  #define BRL_USE_EXTENDED_FONTS 0"
echo "     To:      #define BRL_USE_EXTENDED_FONTS 1"
echo "  3. Rebuild the project"
