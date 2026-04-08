#pragma once
#include <lvgl.h>

// ---------------------------------------------------------------------------
// Layout constants  (1024 x 600 display)
// ---------------------------------------------------------------------------
#define BRL_SCREEN_W    1024
#define BRL_SCREEN_H    600
#define BRL_STATUSBAR_H  40
#define BRL_NAVBAR_H     60
#define BRL_CONTENT_Y   BRL_STATUSBAR_H
#define BRL_CONTENT_H   (BRL_SCREEN_H - BRL_STATUSBAR_H - BRL_NAVBAR_H)
#define BRL_NAVBAR_Y    (BRL_SCREEN_H - BRL_NAVBAR_H)

// ---------------------------------------------------------------------------
// Bavarian RaceLabs LLC — Brand Color Palette
// Primary: Electric Blue  (#0096FF) — high contrast on black
// ---------------------------------------------------------------------------
#define BRL_CLR_BG          lv_color_hex(0x000000)   // pure black background
#define BRL_CLR_SURFACE     lv_color_hex(0x0D1117)   // dark blue-tinted surface
#define BRL_CLR_SURFACE2    lv_color_hex(0x161B22)   // elevated surface
#define BRL_CLR_BORDER      lv_color_hex(0x1C3A5C)   // blue-tinted border

// -- Brand accent: Bavarian electric blue -----------------------------------
#define BRL_CLR_ACCENT      lv_color_hex(0x0096FF)   // BRL Blue -- active / GPS ok
#define BRL_CLR_ACCENT_DIM  lv_color_hex(0x0060C0)   // darker blue for pressed states
// ---------------------------------------------------------------------------

#define BRL_CLR_FASTER      lv_color_hex(0x00C8FF)   // cyan-blue = faster than best
#define BRL_CLR_WARN        lv_color_hex(0xFF9500)   // orange -- slower
#define BRL_CLR_DANGER      lv_color_hex(0xFF3B30)   // red -- much slower / error
#define BRL_CLR_TEXT        lv_color_hex(0xFFFFFF)   // primary text
#define BRL_CLR_TEXT_DIM    lv_color_hex(0x7A8FA6)   // secondary / dim text (blue-grey)
#define BRL_CLR_TEXT_DARK   lv_color_hex(0x3A4A5C)   // inactive nav items
#define BRL_CLR_NAV_BG      lv_color_hex(0x060A0F)   // nav bar background
#define BRL_CLR_STATUSBAR   lv_color_hex(0x030508)   // status bar background

// ---------------------------------------------------------------------------
// Helper: apply dark card style to any container
// ---------------------------------------------------------------------------
static inline void brl_style_card(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj, 8, LV_STATE_DEFAULT);
}

// Helper: transparent background (no border, no bg, no shadow)
static inline void brl_style_transparent(lv_obj_t *obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj, 0, LV_STATE_DEFAULT);
}

// Helper: base label style (white text, default font)
static inline void brl_style_label(lv_obj_t *label, const lv_font_t *font,
                                    lv_color_t color) {
    lv_obj_set_style_text_font(label, font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, color, LV_STATE_DEFAULT);
}

// Helper: apply base button style (no shadow, no border, no outline)
static inline void brl_style_btn(lv_obj_t *btn, lv_color_t bg,
                                  int radius = 6) {
    lv_obj_set_style_bg_color(btn, bg, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, radius, LV_STATE_DEFAULT);
}
