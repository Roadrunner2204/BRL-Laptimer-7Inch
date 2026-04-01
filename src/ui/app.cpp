/**
 * BRL Laptimer — Application UI
 *
 * Screen layout (800 x 480)
 * ┌────────────────────── Status Bar (40px) ───────────────────────┐
 * │                    Content area (380px)                        │
 * ├────────────────────── Nav Bar (60px) ──────────────────────────┤
 *
 * Four panels (one visible at a time):
 *   0 = Dashboard   1 = Tracks   2 = History   3 = Settings
 */

#include <Arduino.h>
#include <lvgl.h>
#include "app.h"
#include "theme.h"
#include "screen_splash.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include "../timing/lap_timer.h"

// ---------------------------------------------------------------------------
// Global application state
// ---------------------------------------------------------------------------
AppState g_state = {};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void build_statusbar(lv_obj_t *parent);
static void build_navbar(lv_obj_t *parent);
static void build_dashboard(lv_obj_t *parent);
static void build_tracks(lv_obj_t *parent);
static void build_history(lv_obj_t *parent);
static void build_settings(lv_obj_t *parent);
static void nav_show(int idx);
static void cb_nav(lv_event_t *e);
static void cb_start_stop(lv_event_t *e);
static void cb_track_select(lv_event_t *e);
static void timer_live_update(lv_timer_t *t);

// ---------------------------------------------------------------------------
// UI object handles
// ---------------------------------------------------------------------------
static lv_obj_t *panels[4];      // content panels
static lv_obj_t *nav_btns[4];    // bottom nav buttons
static int        active_panel = 0;

// Status bar labels
static lv_obj_t *sb_gps_lbl;
static lv_obj_t *sb_wifi_lbl;
static lv_obj_t *sb_obd_lbl;
static lv_obj_t *sb_time_lbl;
static lv_obj_t *sb_track_lbl;

// Dashboard labels (updated by timer)
static lv_obj_t *dash_speed_lbl;
static lv_obj_t *dash_speed_unit;
static lv_obj_t *dash_laptime_lbl;
static lv_obj_t *dash_bestlap_lbl;
static lv_obj_t *dash_lap_count;
static lv_obj_t *dash_sec1_lbl;
static lv_obj_t *dash_sec2_lbl;
static lv_obj_t *dash_sec3_lbl;
static lv_obj_t *dash_delta_lbl;
static lv_obj_t *dash_start_btn_lbl;

// ============================================================================
// SECTION 1 — STATUS BAR
// ============================================================================
static void build_statusbar(lv_obj_t *root) {
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, BRL_SCREEN_W, BRL_STATUSBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, BRL_CLR_STATUSBAR, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(bar, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // --- GPS icon + satellite count ---
    sb_gps_lbl = lv_label_create(bar);
    lv_label_set_text(sb_gps_lbl, LV_SYMBOL_GPS " 0/0");
    brl_style_label(sb_gps_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(sb_gps_lbl, 8, 12);

    // --- WiFi ---
    sb_wifi_lbl = lv_label_create(bar);
    lv_label_set_text(sb_wifi_lbl, LV_SYMBOL_WIFI " --");
    brl_style_label(sb_wifi_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(sb_wifi_lbl, 140, 12);

    // --- OBD ---
    sb_obd_lbl = lv_label_create(bar);
    lv_label_set_text(sb_obd_lbl, LV_SYMBOL_BLUETOOTH " OBD --");
    brl_style_label(sb_obd_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(sb_obd_lbl, 270, 12);

    // --- Active track name (center) ---
    sb_track_lbl = lv_label_create(bar);
    lv_label_set_text(sb_track_lbl, "Keine Strecke");
    brl_style_label(sb_track_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(sb_track_lbl, LV_ALIGN_CENTER, 0, 0);

    // --- Clock (right side) ---
    sb_time_lbl = lv_label_create(bar);
    lv_label_set_text(sb_time_lbl, "00:00");
    brl_style_label(sb_time_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT);
    lv_obj_align(sb_time_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
}

// ============================================================================
// SECTION 2 — NAVIGATION BAR
// ============================================================================
static const char *NAV_ICONS[]  = { LV_SYMBOL_HOME, LV_SYMBOL_LIST,
                                     LV_SYMBOL_DRIVE, LV_SYMBOL_SETTINGS };
static const char *NAV_LABELS[] = { "Fahrt", "Strecken", "Verlauf", "Einstellungen" };

static void build_navbar(lv_obj_t *root) {
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, BRL_SCREEN_W, BRL_NAVBAR_H);
    lv_obj_set_pos(bar, 0, BRL_NAVBAR_Y);
    lv_obj_set_style_bg_color(bar, BRL_CLR_NAV_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(bar, BRL_CLR_BORDER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(bar, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int btn_w = BRL_SCREEN_W / 4;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_obj_create(bar);
        lv_obj_set_size(btn, btn_w, BRL_NAVBAR_H);
        lv_obj_set_pos(btn, i * btn_w, 0);
        brl_style_transparent(btn);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, cb_nav, LV_EVENT_CLICKED, NULL);

        // Icon + label stacked vertically
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, NAV_ICONS[i]);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(icon,
            (i == 0) ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, NAV_LABELS[i]);
        brl_style_label(lbl, &lv_font_montserrat_14,
            (i == 0) ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

        nav_btns[i] = btn;
    }
}

// Update nav bar highlight
static void nav_show(int idx) {
    for (int i = 0; i < 4; i++) {
        lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);

        // Reset nav item colors
        lv_obj_t *icon = lv_obj_get_child(nav_btns[i], 0);
        lv_obj_t *lbl  = lv_obj_get_child(nav_btns[i], 1);
        lv_color_t c = (i == idx) ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM;
        lv_obj_set_style_text_color(icon, c, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(lbl,  c, LV_STATE_DEFAULT);
    }
    lv_obj_remove_flag(panels[idx], LV_OBJ_FLAG_HIDDEN);
    active_panel = idx;
}

static void cb_nav(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    nav_show(idx);
}

// ============================================================================
// SECTION 3 — DASHBOARD
// ============================================================================
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    brl_style_card(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *make_metric_card(lv_obj_t *parent, int x, int y, int w, int h,
                                    const char *title, lv_obj_t **value_out) {
    lv_obj_t *card = make_card(parent, x, y, w, h);

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    brl_style_label(title_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *val = lv_label_create(card);
    lv_label_set_text(val, "---");
    brl_style_label(val, &lv_font_montserrat_32, BRL_CLR_TEXT);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (value_out) *value_out = val;
    return card;
}

static void build_dashboard(lv_obj_t *parent) {
    // ---- Top info row: track name + lap counter ----
    lv_obj_t *info_row = lv_obj_create(parent);
    lv_obj_set_size(info_row, BRL_SCREEN_W, 36);
    lv_obj_set_pos(info_row, 0, 0);
    brl_style_transparent(info_row);
    lv_obj_remove_flag(info_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *track_name = lv_label_create(info_row);
    lv_label_set_text(track_name, "Keine Strecke gewählt");
    brl_style_label(track_name, &lv_font_montserrat_16, BRL_CLR_TEXT_DIM);
    lv_obj_align(track_name, LV_ALIGN_LEFT_MID, 8, 0);

    dash_lap_count = lv_label_create(info_row);
    lv_label_set_text(dash_lap_count, "RUNDE --");
    brl_style_label(dash_lap_count, &lv_font_montserrat_16, BRL_CLR_TEXT_DIM);
    lv_obj_align(dash_lap_count, LV_ALIGN_RIGHT_MID, -8, 0);

    // ---- Speed display (left, large) ----
    lv_obj_t *speed_card = make_card(parent, 8, 42, 300, 140);

    lv_obj_t *speed_title = lv_label_create(speed_card);
    lv_label_set_text(speed_title, "GESCHWINDIGKEIT");
    brl_style_label(speed_title, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(speed_title, LV_ALIGN_TOP_LEFT, 0, 0);

    dash_speed_lbl = lv_label_create(speed_card);
    lv_label_set_text(dash_speed_lbl, "0");
    brl_style_label(dash_speed_lbl, &lv_font_montserrat_48, BRL_CLR_TEXT);
    lv_obj_align(dash_speed_lbl, LV_ALIGN_CENTER, -20, 8);

    dash_speed_unit = lv_label_create(speed_card);
    lv_label_set_text(dash_speed_unit, "km/h");
    brl_style_label(dash_speed_unit, &lv_font_montserrat_20, BRL_CLR_TEXT_DIM);
    lv_obj_align_to(dash_speed_unit, dash_speed_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    // ---- Lap time (center, large) ----
    lv_obj_t *laptime_card = make_card(parent, 316, 42, 300, 140);

    lv_obj_t *lt_title = lv_label_create(laptime_card);
    lv_label_set_text(lt_title, "RUNDENZEIT");
    brl_style_label(lt_title, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(lt_title, LV_ALIGN_TOP_LEFT, 0, 0);

    dash_laptime_lbl = lv_label_create(laptime_card);
    lv_label_set_text(dash_laptime_lbl, "-:--:---");
    brl_style_label(dash_laptime_lbl, &lv_font_montserrat_40, BRL_CLR_ACCENT);
    lv_obj_align(dash_laptime_lbl, LV_ALIGN_CENTER, 0, 8);

    // ---- Best lap (right) ----
    lv_obj_t *best_card = make_card(parent, 624, 42, 168, 140);

    lv_obj_t *bl_title = lv_label_create(best_card);
    lv_label_set_text(bl_title, "BESTZEIT");
    brl_style_label(bl_title, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(bl_title, LV_ALIGN_TOP_LEFT, 0, 0);

    dash_bestlap_lbl = lv_label_create(best_card);
    lv_label_set_text(dash_bestlap_lbl, "-:--:---");
    brl_style_label(dash_bestlap_lbl, &lv_font_montserrat_24, BRL_CLR_TEXT);
    lv_obj_align(dash_bestlap_lbl, LV_ALIGN_CENTER, 0, 8);

    // ---- Sector times row ----
    int sec_y = 190;
    int sec_h = 90;
    int sec_w = 182;

    make_metric_card(parent, 8,   sec_y, sec_w, sec_h, "SEKTOR 1", &dash_sec1_lbl);
    make_metric_card(parent, 198, sec_y, sec_w, sec_h, "SEKTOR 2", &dash_sec2_lbl);
    make_metric_card(parent, 388, sec_y, sec_w, sec_h, "SEKTOR 3", &dash_sec3_lbl);
    lv_obj_set_style_text_font(dash_sec1_lbl, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(dash_sec2_lbl, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(dash_sec3_lbl, &lv_font_montserrat_24, LV_STATE_DEFAULT);

    // ---- Delta card ----
    lv_obj_t *delta_card = make_card(parent, 578, sec_y, 214, sec_h);

    lv_obj_t *delta_title = lv_label_create(delta_card);
    lv_label_set_text(delta_title, "DIFF ZUR BESTZEIT");
    brl_style_label(delta_title, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(delta_title, LV_ALIGN_TOP_LEFT, 0, 0);

    dash_delta_lbl = lv_label_create(delta_card);
    lv_label_set_text(dash_delta_lbl, "---");
    brl_style_label(dash_delta_lbl, &lv_font_montserrat_24, BRL_CLR_TEXT_DIM);
    lv_obj_align(dash_delta_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // ---- OBD mini row ----
    int obd_y = 288;
    int obd_h = 52;
    int obd_w = 120;

    lv_obj_t *obd_row_bg = make_card(parent, 8, obd_y, 560, obd_h);
    brl_style_transparent(obd_row_bg);
    lv_obj_remove_flag(obd_row_bg, LV_OBJ_FLAG_SCROLLABLE);

    struct { const char *label; int x; } obd_items[] = {
        { "GANG",   0   },
        { "RPM",    140 },
        { "GAS",    280 },
        { "KÜHLMITTEL", 420 },
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = make_card(obd_row_bg, obd_items[i].x, 0, obd_w, obd_h);

        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, obd_items[i].label);
        brl_style_label(t, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *v = lv_label_create(c);
        lv_label_set_text(v, "--");
        brl_style_label(v, &lv_font_montserrat_20, BRL_CLR_TEXT);
        lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    // ---- Start / Stop button ----
    lv_obj_t *start_btn = lv_button_create(parent);
    lv_obj_set_size(start_btn, 210, 52);
    lv_obj_set_pos(start_btn, 578, obd_y);
    lv_obj_set_style_bg_color(start_btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(start_btn, BRL_CLR_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_radius(start_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(start_btn, 0, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(start_btn, cb_start_stop, LV_EVENT_CLICKED, NULL);

    dash_start_btn_lbl = lv_label_create(start_btn);
    lv_label_set_text(dash_start_btn_lbl, LV_SYMBOL_PLAY "  TIMING STARTEN");
    brl_style_label(dash_start_btn_lbl, &lv_font_montserrat_16,
                    BRL_CLR_TEXT);   // weißer Text auf BRL-Blau
    lv_obj_center(dash_start_btn_lbl);
}

// ============================================================================
// SECTION 4 — TRACK SELECTION
// ============================================================================
static void cb_track_select(lv_event_t *e) {
    lv_obj_t *item = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(item);
    g_state.active_track_idx = idx;

    const TrackDef *td = track_get(idx);
    if (td) lv_label_set_text(sb_track_lbl, td->name);

    // Apply track to lap timer
    lap_timer_set_track(idx);

    nav_show(0);
}

static void build_tracks(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Strecke wählen");
    brl_style_label(title, &lv_font_montserrat_24, BRL_CLR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "Tippe auf eine Strecke um sie als aktiv zu setzen.");
    brl_style_label(hint, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    // Scrollable list
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, BRL_SCREEN_W - 16, BRL_CONTENT_H - 72);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(list, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(list, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(list, 0, LV_STATE_DEFAULT);

    for (int i = 0; i < track_total_count(); i++) {
        const TrackDef *td = track_get(i);
        if (!td) continue;
        char label_text[80];
        snprintf(label_text, sizeof(label_text), "%s   %.3f km   %s",
                 td->name, td->length_km, td->country);

        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_RIGHT, label_text);
        lv_obj_set_style_bg_color(btn, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, BRL_CLR_SURFACE2, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(btn, 6, LV_STATE_DEFAULT);
        lv_obj_set_height(btn, 46);

        // Style the label inside the list button
        lv_obj_t *lbl = lv_obj_get_child(btn, 1);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, BRL_CLR_TEXT, LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_STATE_DEFAULT);
        }
        lv_obj_t *icon = lv_obj_get_child(btn, 0);
        if (icon) {
            lv_obj_set_style_text_color(icon, BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);
        }

        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, cb_track_select, LV_EVENT_CLICKED, NULL);
    }
}

// ============================================================================
// SECTION 5 — HISTORY
// ============================================================================
static void build_history(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Rundenzeiten");
    brl_style_label(title, &lv_font_montserrat_24, BRL_CLR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *placeholder = lv_label_create(parent);
    lv_label_set_text(placeholder, "Noch keine Rundenzeiten aufgezeichnet.");
    brl_style_label(placeholder, &lv_font_montserrat_16, BRL_CLR_TEXT_DIM);
    lv_obj_align(placeholder, LV_ALIGN_CENTER, 0, 0);
}

// ============================================================================
// SECTION 6 — SETTINGS
// ============================================================================
static void build_settings(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Einstellungen");
    brl_style_label(title, &lv_font_montserrat_24, BRL_CLR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *list = lv_list_create(parent);
    lv_obj_set_size(list, BRL_SCREEN_W - 16, BRL_CONTENT_H - 60);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(list, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(list, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(list, 0, LV_STATE_DEFAULT);

    struct { const char *icon; const char *label; const char *value; } items[] = {
        { LV_SYMBOL_SETTINGS,  "Sprache",                   "Deutsch"       },
        { LV_SYMBOL_WIFI,      "WLAN",                      "Nicht verbunden" },
        { LV_SYMBOL_BLUETOOTH, "OBD Bluetooth",             "Nicht verbunden" },
        { LV_SYMBOL_GPS,       "GPS Modul",                 "NEO-6M"        },
        { LV_SYMBOL_LOOP,      "Einheiten",                 "km/h"          },
        { LV_SYMBOL_DOWNLOAD,  "Updates prüfen",            "v1.0.0"        },
        { LV_SYMBOL_WIFI,      "Daten per WLAN exportieren","Android App"   },
        { LV_SYMBOL_SETTINGS,  "Über BRL Laptimer",         ""              },
    };

    for (int i = 0; i < (int)(sizeof(items)/sizeof(items[0])); i++) {
        char row_text[80];
        snprintf(row_text, sizeof(row_text), "%-28s %s", items[i].label, items[i].value);

        lv_obj_t *btn = lv_list_add_button(list, items[i].icon, row_text);
        lv_obj_set_style_bg_color(btn, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, BRL_CLR_SURFACE2, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(btn, 6, LV_STATE_DEFAULT);
        lv_obj_set_height(btn, 48);

        lv_obj_t *lbl = lv_obj_get_child(btn, 1);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, BRL_CLR_TEXT, LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_STATE_DEFAULT);
        }
        lv_obj_t *icon = lv_obj_get_child(btn, 0);
        if (icon) {
            lv_obj_set_style_text_color(icon, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
        }
    }
}

// ============================================================================
// SECTION 7 — LIVE UPDATE TIMER
// ============================================================================
static void cb_start_stop(lv_event_t *e) {
    LiveTiming &lt = g_state.timing;
    if (!lt.timing_active) {
        if (g_state.active_track_idx < 0) return; // no track selected
        // GPS timing is fully automatic — just arm it
        lt.timing_active = true;
        lt.lap_number    = 0;
        lv_label_set_text(dash_start_btn_lbl, LV_SYMBOL_STOP "  TIMING STOPPEN");
        lv_obj_set_style_bg_color(
            lv_obj_get_parent(dash_start_btn_lbl),
            BRL_CLR_DANGER, LV_STATE_DEFAULT);
    } else {
        lt.timing_active = false;
        lv_label_set_text(dash_start_btn_lbl, LV_SYMBOL_PLAY "  TIMING STARTEN");
        lv_obj_set_style_bg_color(
            lv_obj_get_parent(dash_start_btn_lbl),
            BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    }
}

static void timer_live_update(lv_timer_t * /*t*/) {
    // --- GPS status ---
    char gps_buf[24];
    snprintf(gps_buf, sizeof(gps_buf), LV_SYMBOL_GPS " %d/%d",
             g_state.gps.satellites, 12);
    lv_label_set_text(sb_gps_lbl, gps_buf);
    lv_obj_set_style_text_color(sb_gps_lbl,
        g_state.gps.valid ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);

    // --- WiFi status ---
    bool wifi_on = (g_state.wifi_mode != BRL_WIFI_OFF);
    lv_label_set_text(sb_wifi_lbl,
        wifi_on ? LV_SYMBOL_WIFI " OK" : LV_SYMBOL_WIFI " --");
    lv_obj_set_style_text_color(sb_wifi_lbl,
        wifi_on ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);

    // --- OBD status ---
    lv_label_set_text(sb_obd_lbl,
        g_state.obd.connected
            ? LV_SYMBOL_BLUETOOTH " OBD OK"
            : LV_SYMBOL_BLUETOOTH " OBD --");
    lv_obj_set_style_text_color(sb_obd_lbl,
        g_state.obd.connected ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);

    // --- Speed ---
    char spd[8];
    int spd_val = (int)g_state.gps.speed_kmh;
    snprintf(spd, sizeof(spd), "%d", spd_val);
    lv_label_set_text(dash_speed_lbl, spd);

    // --- Lap timer (counting up) ---
    LiveTiming  &lt   = g_state.timing;
    LapSession  &sess = g_state.session;

    if (lt.timing_active && lt.in_lap) {
        uint32_t elapsed = millis() - lt.lap_start_ms;
        char lt_buf[16];
        fmt_laptime(lt_buf, sizeof(lt_buf), elapsed);
        lv_label_set_text(dash_laptime_lbl, lt_buf);

        // Live delta vs reference lap
        char delta_buf[12];
        fmt_delta(delta_buf, sizeof(delta_buf), lt.live_delta_ms);
        lv_label_set_text(dash_delta_lbl, delta_buf);
        lv_obj_set_style_text_color(dash_delta_lbl,
            lt.live_delta_ms <= 0 ? BRL_CLR_FASTER : BRL_CLR_WARN, LV_STATE_DEFAULT);

        // Lap counter
        char lap_buf[16];
        snprintf(lap_buf, sizeof(lap_buf), "RUNDE %d", lt.lap_number);
        lv_label_set_text(dash_lap_count, lap_buf);
        lv_obj_set_style_text_color(dash_lap_count, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    }

    // --- Best lap ---
    uint8_t bi = sess.best_lap_idx;
    if (sess.lap_count > 0 && sess.laps[bi].valid) {
        char bl_buf[16];
        fmt_laptime(bl_buf, sizeof(bl_buf), sess.laps[bi].total_ms);
        lv_label_set_text(dash_bestlap_lbl, bl_buf);
    }
}

// ============================================================================
// SECTION 8 — MAIN UI (called after splash)
// ============================================================================
static void build_main_ui() {
    // Eigenen neuen LVGL-Screen anlegen (nicht lv_screen_active() nehmen —
    // das wäre noch der Splash-Screen, der gleich danach gelöscht wird)
    lv_obj_t *root = lv_obj_create(nullptr);

    // Black background
    lv_obj_set_style_bg_color(root, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // Build panels (content areas)
    for (int i = 0; i < 4; i++) {
        panels[i] = lv_obj_create(root);
        lv_obj_set_size(panels[i], BRL_SCREEN_W, BRL_CONTENT_H);
        lv_obj_set_pos(panels[i], 0, BRL_CONTENT_Y);
        lv_obj_set_style_bg_color(panels[i], BRL_CLR_BG, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(panels[i], LV_OPA_COVER, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(panels[i], 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(panels[i], 0, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(panels[i], 6, LV_STATE_DEFAULT);
        lv_obj_remove_flag(panels[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Build screen contents
    build_dashboard(panels[0]);
    build_tracks(panels[1]);
    build_history(panels[2]);
    build_settings(panels[3]);

    // Build chrome (drawn on top so they're always visible)
    build_statusbar(root);
    build_navbar(root);

    // Neuen Screen aktivieren (Splash wird danach von screen_splash.cpp gelöscht)
    lv_screen_load(root);

    // Show dashboard
    nav_show(0);

    // Periodic live-update timer (100 ms)
    lv_timer_create(timer_live_update, 100, NULL);
}

// ============================================================================
// SECTION 9 — PUBLIC ENTRY POINT
// ============================================================================
void app_init() {
    g_state = {};
    g_state.active_track_idx = -1;
    g_state.language = 0;   // DE
    g_state.units    = 0;   // km/h

    // Splash-Screen zuerst zeigen (3 Sekunden), dann Haupt-UI aufbauen
    splash_show(3000, build_main_ui);
}

void app_tick() {
    // GPS und OBD hier pollen (wird von main.cpp loop() aufgerufen)
}
