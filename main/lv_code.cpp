/**
 * lv_code.cpp — LVGL UI entry point
 *
 * Called once from main.c after LVGL is fully initialised.
 * Delegates to app_init() in ui/app.cpp.
 */

#include "lvgl.h"
#include "ui/app.h"
#include "ui/brl_fonts.h"

/**
 * Wire up fallback fonts so that LV_SYMBOL_* glyphs (Font Awesome range
 * U+F000–U+F8FF) are found in the built-in LVGL Montserrat fonts, while
 * the custom BRL fonts still handle Latin/extended-Latin text.
 */
static void brl_fonts_set_fallbacks(void) {
    /* lv_font_t.fallback is a const pointer, but the built-in fonts are
       linked in read-write .data — safe to patch once before any UI runs. */
    ((lv_font_t *)&brl_font_montserrat_14)->fallback = &lv_font_montserrat_14;
    ((lv_font_t *)&brl_font_montserrat_16)->fallback = &lv_font_montserrat_16;
    ((lv_font_t *)&brl_font_montserrat_20)->fallback = &lv_font_montserrat_20;
    ((lv_font_t *)&brl_font_montserrat_24)->fallback = &lv_font_montserrat_24;
    ((lv_font_t *)&brl_font_montserrat_32)->fallback = &lv_font_montserrat_32;
    ((lv_font_t *)&brl_font_montserrat_40)->fallback = &lv_font_montserrat_40;
    ((lv_font_t *)&brl_font_montserrat_48)->fallback = &lv_font_montserrat_48;
    /* BRL_FONT_64: no matching built-in; fall back to 48 */
    ((lv_font_t *)&brl_font_montserrat_64)->fallback = &lv_font_montserrat_48;
}

extern "C" void lv_my_setup(void) {
    brl_fonts_set_fallbacks();
    app_init();
}
