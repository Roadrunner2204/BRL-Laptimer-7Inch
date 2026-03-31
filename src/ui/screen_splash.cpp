/**
 * screen_splash.cpp — Boot Splash-Screen
 *
 * Zeigt beim Start:
 *   - Bavarian RaceLabs Logo (JPEG, 225x225 px)
 *   - "BRL LAPTIMER" Schriftzug
 *   - Animierter Ladebalken (Bavarian Blue)
 *   - Version + Copyright
 */

#include <Arduino.h>
#include <lvgl.h>
#include "screen_splash.h"
#include "theme.h"
#include "../assets/logo_data.h"

#define APP_VERSION "v1.0.0"

static void (*s_on_done)()   = nullptr;
static lv_obj_t   *s_screen = nullptr;
static lv_obj_t   *s_bar    = nullptr;
static lv_timer_t *s_timer  = nullptr;
static uint32_t    s_duration_ms = 0;
static uint32_t    s_start_ms    = 0;

// ---------------------------------------------------------------------------
// JPEG image descriptor — LVGL TJPGD dekodiert es on-the-fly
// ---------------------------------------------------------------------------
static const lv_image_dsc_t logo_dsc = {
    .header = {
        .magic    = LV_IMAGE_HEADER_MAGIC,
        .cf       = LV_COLOR_FORMAT_RAW,   // TJPGD decoder
        .flags    = 0,
        .w        = LOGO_JPG_WIDTH,
        .h        = LOGO_JPG_HEIGHT,
        .stride   = 0,
    },
    .data_size = LOGO_JPG_SIZE,
    .data      = LOGO_JPG,
};

// ---------------------------------------------------------------------------
// Timer: Ladebalken animieren, dann Übergang zur Haupt-UI
// ---------------------------------------------------------------------------
static void cb_progress(lv_timer_t * /*t*/) {
    uint32_t elapsed = millis() - s_start_ms;
    int32_t  pct     = (int32_t)((elapsed * 100) / s_duration_ms);
    if (pct > 100) pct = 100;

    lv_bar_set_value(s_bar, pct, LV_ANIM_ON);

    if (pct >= 100) {
        lv_timer_del(s_timer);
        s_timer = nullptr;

        // Reihenfolge wichtig:
        // 1. on_done() aufrufen → build_main_ui() erstellt + lädt neuen Screen
        // 2. DANACH Splash-Screen löschen (neuer Screen ist schon aktiv)
        void (*cb)() = s_on_done;
        lv_obj_t *splash = s_screen;
        s_on_done = nullptr;
        s_screen  = nullptr;

        if (cb) cb();                          // neuer Screen wird geladen
        if (splash) lv_obj_delete(splash);     // alter Splash wird gelöscht
    }
}

// ---------------------------------------------------------------------------
// Splash-Screen aufbauen und anzeigen
// ---------------------------------------------------------------------------
void splash_show(uint32_t duration_ms, void (*on_done)()) {
    s_on_done     = on_done;
    s_duration_ms = duration_ms;
    s_start_ms    = millis();

    // Eigener unabhängiger LVGL-Screen
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_size(s_screen, BRL_SCREEN_W, BRL_SCREEN_H);
    lv_obj_set_style_bg_color(s_screen, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_screen, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_screen);

    // --- Logo (JPEG, 225x225, zentriert oben) ---
    lv_obj_t *img = lv_image_create(s_screen);
    lv_image_set_src(img, &logo_dsc);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -60);

    // --- "BAVARIAN RACELABS" Schriftzug ---
    lv_obj_t *brand = lv_label_create(s_screen);
    lv_label_set_text(brand, "BAVARIAN RACELABS");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(brand, BRL_CLR_TEXT, LV_STATE_DEFAULT);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, 80);

    // --- "LAPTIMER" Untertitel ---
    lv_obj_t *sub = lv_label_create(s_screen);
    lv_label_set_text(sub, "L A P T I M E R");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sub, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 110);

    // --- Ladebalken ---
    s_bar = lv_bar_create(s_screen);
    lv_obj_set_size(s_bar, 360, 6);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_set_style_bg_color(s_bar, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 3, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_bar, BRL_CLR_ACCENT,
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 3,
                             LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    // --- "Wird gestartet..." ---
    lv_obj_t *loading = lv_label_create(s_screen);
    lv_label_set_text(loading, "Wird gestartet...");
    lv_obj_set_style_text_font(loading, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(loading, BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);
    lv_obj_align(loading, LV_ALIGN_BOTTOM_MID, 0, -70);

    // --- Version (rechts unten) ---
    lv_obj_t *ver = lv_label_create(s_screen);
    lv_label_set_text(ver, APP_VERSION);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ver, BRL_CLR_TEXT_DARK, LV_STATE_DEFAULT);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

    // --- Copyright (links unten) ---
    lv_obj_t *copy = lv_label_create(s_screen);
    lv_label_set_text(copy, "Bavarian RaceLabs LLC");
    lv_obj_set_style_text_font(copy, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(copy, BRL_CLR_TEXT_DARK, LV_STATE_DEFAULT);
    lv_obj_align(copy, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    // --- Fortschritts-Timer (50ms Interval) ---
    s_timer = lv_timer_create(cb_progress, 50, nullptr);
}
