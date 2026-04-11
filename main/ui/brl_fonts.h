#pragma once
/**
 * brl_fonts.h — Custom Montserrat fonts with extended Latin (Ä Ö Ü ä ö ü ß €)
 *
 * Generated with lv_font_conv for LVGL 9:
 *   npx lv_font_conv --bpp 4 --size N --font Montserrat-Regular.ttf \
 *     --range 0x20-0x7F,0xA0-0xFF,0x2013-0x2014,0x2022,0x20AC \
 *     --format lvgl --lv-include "lvgl.h" -o brl_font_montserrat_N.c
 */

#include <lvgl.h>

// Non-const so fallback pointer can be patched at runtime (lv_code.cpp)
extern lv_font_t brl_font_montserrat_14;
extern lv_font_t brl_font_montserrat_16;
extern lv_font_t brl_font_montserrat_20;
extern lv_font_t brl_font_montserrat_24;
extern lv_font_t brl_font_montserrat_32;
extern lv_font_t brl_font_montserrat_40;
extern lv_font_t brl_font_montserrat_48;
extern lv_font_t brl_font_montserrat_64;
extern lv_font_t brl_font_montserrat_96;
extern lv_font_t brl_font_montserrat_128;
extern lv_font_t brl_font_montserrat_160;

#define BRL_FONT_14  brl_font_montserrat_14
#define BRL_FONT_16  brl_font_montserrat_16
#define BRL_FONT_20  brl_font_montserrat_20
#define BRL_FONT_24  brl_font_montserrat_24
#define BRL_FONT_32  brl_font_montserrat_32
#define BRL_FONT_40  brl_font_montserrat_40
#define BRL_FONT_48  brl_font_montserrat_48
#define BRL_FONT_64  brl_font_montserrat_64
#define BRL_FONT_96  brl_font_montserrat_96
#define BRL_FONT_128 brl_font_montserrat_128
#define BRL_FONT_160 brl_font_montserrat_160
