#pragma once
/**
 * brl_fonts.h — Font declarations for BRL Laptimer
 *
 * The custom fonts (with Ä Ö Ü ä ö ü ß) were generated for LVGL 8 and
 * are NOT compatible with LVGL 9. Until they are regenerated with
 * lv_font_conv for LVGL 9, we fall back to the built-in Montserrat fonts.
 *
 * TODO: Regenerate fonts with:
 *   npx lv_font_conv --bpp 4 --size 14 --font Montserrat-Regular.ttf \
 *     --range 0x20-0x7F,0xC0-0xFF --format lvgl9 -o brl_font_montserrat_14.c
 *   (repeat for each size: 14, 16, 20, 24, 32, 40, 48, 64)
 */

#include <lvgl.h>

/* Fall back to LVGL 9 built-in Montserrat fonts (ASCII only, no umlauts) */
#define BRL_FONT_14  lv_font_montserrat_14
#define BRL_FONT_16  lv_font_montserrat_16
#define BRL_FONT_20  lv_font_montserrat_20
#define BRL_FONT_24  lv_font_montserrat_24
#define BRL_FONT_32  lv_font_montserrat_32
#define BRL_FONT_40  lv_font_montserrat_40
#define BRL_FONT_48  lv_font_montserrat_48
#define BRL_FONT_64  lv_font_montserrat_48  /* no built-in 64, use 48 */
