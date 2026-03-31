/**
 * screen_splash.cpp — Boot Splash-Screen
 *
 * Zeigt beim Start:
 *   - Dein Logo (JPEG) → Platzhalter bis Logo geliefert wird
 *   - "BRL LAPTIMER" Schriftzug
 *   - Animierter Ladebalken
 *   - Version
 *
 * JPEG-Integration (wenn Logo fertig ist):
 *   → Kommentar-Block in build_logo_area() aktivieren
 *   → LOGO_USE_PLACEHOLDER in assets/logo_placeholder.h entfernen
 */

#include <Arduino.h>
#include <lvgl.h>
#include "screen_splash.h"
#include "theme.h"
#include "../assets/logo_placeholder.h"

#define APP_VERSION  "v1.0.0"

static void (*s_on_done)() = nullptr;
static lv_obj_t *s_splash_screen = nullptr;
static lv_obj_t *s_bar = nullptr;
static lv_timer_t *s_timer = nullptr;
static uint32_t s_duration_ms = 0;
static uint32_t s_start_ms = 0;

// ---------------------------------------------------------------------------
// Animierter Ladebalken-Update (alle 50 ms)
// ---------------------------------------------------------------------------
static void cb_progress(lv_timer_t *t) {
    uint32_t elapsed = millis() - s_start_ms;
    int32_t  pct     = (int32_t)((elapsed * 100) / s_duration_ms);
    if (pct > 100) pct = 100;

    lv_bar_set_value(s_bar, pct, LV_ANIM_ON);

    if (pct >= 100) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
        // Kurz warten bis die Balken-Animation fertig ist
        if (s_on_done) {
            lv_timer_t *done_t = lv_timer_create(
                [](lv_timer_t *t2) {
                    lv_timer_del(t2);
                    if (s_splash_screen) {
                        lv_obj_delete(s_splash_screen);
                        s_splash_screen = nullptr;
                    }
                    if (s_on_done) s_on_done();
                },
                300, nullptr);
            lv_timer_set_repeat_count(done_t, 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Logo-Bereich aufbauen
// ---------------------------------------------------------------------------
static void build_logo_area(lv_obj_t *parent) {

#ifdef LOGO_USE_PLACEHOLDER
    // ── Platzhalter: Text-Logo ──────────────────────────────────────────
    lv_obj_t *logo_box = lv_obj_create(parent);
    lv_obj_set_size(logo_box, 420, 160);
    lv_obj_align(logo_box, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_bg_color(logo_box, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(logo_box, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(logo_box, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(logo_box, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(logo_box, 12, LV_STATE_DEFAULT);
    lv_obj_clear_flag(logo_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(logo_box);
    lv_label_set_text(brand, "BRL");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_48, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(brand, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *sub = lv_label_create(logo_box);
    lv_label_set_text(sub, "LAPTIMER");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(sub, BRL_CLR_TEXT, LV_STATE_DEFAULT);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 28);

#else
    // ── Echtes JPEG-Logo ────────────────────────────────────────────────
    // Wird aktiv sobald logo_data.h mit dem JPEG-Array existiert.
    //
    // static const lv_image_dsc_t logo_dsc = {
    //     .header = {
    //         .magic      = LV_IMAGE_HEADER_MAGIC,
    //         .cf         = LV_COLOR_FORMAT_RAW,   // TJPGD dekodiert on-the-fly
    //         .w          = 400,
    //         .h          = 200,
    //     },
    //     .data_size = LOGO_JPG_SIZE,
    //     .data      = LOGO_JPG,
    // };
    //
    // lv_obj_t *img = lv_image_create(parent);
    // lv_image_set_src(img, &logo_dsc);
    // lv_obj_align(img, LV_ALIGN_CENTER, 0, -50);
#endif
}

// ---------------------------------------------------------------------------
// Splash aufbauen
// ---------------------------------------------------------------------------
void splash_show(uint32_t duration_ms, void (*on_done)()) {
    s_on_done    = on_done;
    s_duration_ms = duration_ms;
    s_start_ms   = millis();

    // Eigener LVGL-Screen damit er komplett überschrieben werden kann
    s_splash_screen = lv_obj_create(nullptr);
    lv_obj_set_size(s_splash_screen, BRL_SCREEN_W, BRL_SCREEN_H);
    lv_obj_set_style_bg_color(s_splash_screen, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_splash_screen, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_splash_screen, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_splash_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_splash_screen);

    // --- Logo ---
    build_logo_area(s_splash_screen);

    // --- Ladebalken ---
    s_bar = lv_bar_create(s_splash_screen);
    lv_obj_set_size(s_bar, 400, 8);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_bg_color(s_bar, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 4, LV_STATE_DEFAULT);
    // Indicator (der grüne Fortschritt)
    lv_obj_set_style_bg_color(s_bar, BRL_CLR_ACCENT,
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    // --- "Wird geladen..." Text ---
    lv_obj_t *loading_lbl = lv_label_create(s_splash_screen);
    lv_label_set_text(loading_lbl, "Wird gestartet...");
    lv_obj_set_style_text_font(loading_lbl, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(loading_lbl, BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);
    lv_obj_align(loading_lbl, LV_ALIGN_BOTTOM_MID, 0, -76);

    // --- Version ---
    lv_obj_t *ver_lbl = lv_label_create(s_splash_screen);
    lv_label_set_text(ver_lbl, APP_VERSION);
    lv_obj_set_style_text_font(ver_lbl, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ver_lbl, BRL_CLR_TEXT_DARK, LV_STATE_DEFAULT);
    lv_obj_align(ver_lbl, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

    // --- Copyright ---
    lv_obj_t *copy_lbl = lv_label_create(s_splash_screen);
    lv_label_set_text(copy_lbl, "BRL Motorsport");
    lv_obj_set_style_text_font(copy_lbl, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(copy_lbl, BRL_CLR_TEXT_DARK, LV_STATE_DEFAULT);
    lv_obj_align(copy_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    // --- Fortschritts-Timer ---
    s_timer = lv_timer_create(cb_progress, 50, nullptr);
}
