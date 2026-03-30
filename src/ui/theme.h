#pragma once
#include <lvgl.h>

// ---------------------------------------------------------------------------
// Layout constants  (800 x 480 display)
// ---------------------------------------------------------------------------
#define BRL_SCREEN_W    800
#define BRL_SCREEN_H    480
#define BRL_STATUSBAR_H  40
#define BRL_NAVBAR_H     60
#define BRL_CONTENT_Y   BRL_STATUSBAR_H
#define BRL_CONTENT_H   (BRL_SCREEN_H - BRL_STATUSBAR_H - BRL_NAVBAR_H)
#define BRL_NAVBAR_Y    (BRL_SCREEN_H - BRL_NAVBAR_H)

// ---------------------------------------------------------------------------
// Color palette
// ---------------------------------------------------------------------------
#define BRL_CLR_BG          lv_color_hex(0x000000)   // pure black
#define BRL_CLR_SURFACE     lv_color_hex(0x111111)   // card surface
#define BRL_CLR_SURFACE2    lv_color_hex(0x1E1E1E)   // elevated surface
#define BRL_CLR_BORDER      lv_color_hex(0x2A2A2A)   // subtle border
#define BRL_CLR_ACCENT      lv_color_hex(0x00E676)   // green — good / active
#define BRL_CLR_WARN        lv_color_hex(0xFF9800)   // orange — warning / slower
#define BRL_CLR_DANGER      lv_color_hex(0xF44336)   // red — error / much slower
#define BRL_CLR_TEXT        lv_color_hex(0xFFFFFF)   // primary text
#define BRL_CLR_TEXT_DIM    lv_color_hex(0x888888)   // secondary / dim text
#define BRL_CLR_TEXT_DARK   lv_color_hex(0x444444)   // inactive nav items
#define BRL_CLR_NAV_BG      lv_color_hex(0x0A0A0A)   // nav bar background
#define BRL_CLR_STATUSBAR   lv_color_hex(0x050505)   // status bar background

// ---------------------------------------------------------------------------
// Helper: apply dark card style to any container
// ---------------------------------------------------------------------------
static inline void brl_style_card(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, BRL_CLR_BORDER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj, 8, LV_STATE_DEFAULT);
}

// Helper: transparent background (no border, no bg)
static inline void brl_style_transparent(lv_obj_t *obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(obj, 0, LV_STATE_DEFAULT);
}

// Helper: base label style (white text, default font)
static inline void brl_style_label(lv_obj_t *label, const lv_font_t *font,
                                    lv_color_t color) {
    lv_obj_set_style_text_font(label, font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, color, LV_STATE_DEFAULT);
}
