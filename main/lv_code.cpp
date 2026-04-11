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
    /* BRL fonts are non-const so fallback can be set at runtime.
       Built-in LVGL Montserrat fonts include Font Awesome symbols. */
    brl_font_montserrat_14.fallback = &lv_font_montserrat_14;
    brl_font_montserrat_16.fallback = &lv_font_montserrat_16;
    brl_font_montserrat_20.fallback = &lv_font_montserrat_20;
    brl_font_montserrat_24.fallback = &lv_font_montserrat_24;
    brl_font_montserrat_32.fallback = &lv_font_montserrat_32;
    brl_font_montserrat_40.fallback = &lv_font_montserrat_40;
    brl_font_montserrat_48.fallback = &lv_font_montserrat_48;
    /* BRL_FONT_64: no matching built-in; fall back to 48 */
    brl_font_montserrat_64.fallback = &lv_font_montserrat_48;
}

extern "C" void lv_my_setup(void) {
    brl_fonts_set_fallbacks();
    app_init();
}
