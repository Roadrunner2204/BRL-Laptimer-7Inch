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
#include "../obd/obd_bt.h"
#include "../wifi/wifi_mgr.h"

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

// Settings handles (updated by timer + callbacks)
static lv_obj_t *set_obd_status_lbl;
static lv_obj_t *set_obd_btn;
static lv_obj_t *set_wifi_ap_sw;
static lv_obj_t *set_wifi_ap_status_lbl;
static lv_obj_t *set_wifi_sta_status_lbl;
static lv_obj_t *set_lang_btn;
static lv_obj_t *set_units_btn;

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
// SECTION 6 — SETTINGS  (interactive)
// ============================================================================

// ---------------------------------------------------------------------------
// Helper: create a settings row  (card with icon+title left, control right)
// Returns the right-side container so callers can add controls there.
// ---------------------------------------------------------------------------
static lv_obj_t *make_setting_row(lv_obj_t *parent, int y, int h,
                                   const char *icon, const char *title,
                                   const char *subtitle = nullptr) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, BRL_SCREEN_W - 16, h);
    lv_obj_set_pos(row, 0, y);
    brl_style_card(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Icon
    lv_obj_t *ico = lv_label_create(row);
    lv_label_set_text(ico, icon);
    brl_style_label(ico, &lv_font_montserrat_20, BRL_CLR_ACCENT);
    lv_obj_align(ico, LV_ALIGN_LEFT_MID, 0, subtitle ? -8 : 0);

    // Title
    lv_obj_t *tit = lv_label_create(row);
    lv_label_set_text(tit, title);
    brl_style_label(tit, &lv_font_montserrat_16, BRL_CLR_TEXT);
    lv_obj_align(tit, LV_ALIGN_LEFT_MID, 32, subtitle ? -8 : 0);

    // Optional subtitle
    if (subtitle) {
        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text(sub, subtitle);
        brl_style_label(sub, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 32, 10);
    }

    // Right container for controls
    lv_obj_t *right = lv_obj_create(row);
    lv_obj_set_size(right, 340, h - 16);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    brl_style_transparent(right);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    return right;
}

// ---------------------------------------------------------------------------
// Small action button used inside settings rows
// ---------------------------------------------------------------------------
static lv_obj_t *make_setting_btn(lv_obj_t *parent, const char *text,
                                   lv_color_t color, lv_align_t align,
                                   int x_ofs = 0) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 120, 36);
    lv_obj_align(btn, align, x_ofs, 0);
    lv_obj_set_style_bg_color(btn, color, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, BRL_CLR_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    brl_style_label(lbl, &lv_font_montserrat_14, BRL_CLR_TEXT);
    lv_obj_center(lbl);
    return btn;
}

// ---------------------------------------------------------------------------
// WiFi credential dialog (modal overlay over root screen)
// ---------------------------------------------------------------------------
static lv_obj_t *s_wifi_dialog    = nullptr;
static lv_obj_t *s_ta_ssid        = nullptr;
static lv_obj_t *s_ta_pass        = nullptr;
static lv_obj_t *s_kb             = nullptr;

static void cb_wifi_dialog_save(lv_event_t * /*e*/) {
    if (!s_ta_ssid || !s_ta_pass) return;
    wifi_set_sta(lv_textarea_get_text(s_ta_ssid),
                 lv_textarea_get_text(s_ta_pass));
    // Switch to STA mode immediately
    wifi_set_mode(BRL_WIFI_STA);

    if (s_wifi_dialog) { lv_obj_delete(s_wifi_dialog); s_wifi_dialog = nullptr; }
}

static void cb_wifi_dialog_cancel(lv_event_t * /*e*/) {
    if (s_wifi_dialog) { lv_obj_delete(s_wifi_dialog); s_wifi_dialog = nullptr; }
}

static void cb_kb_ready(lv_event_t *e) {
    // Keyboard OK button closes it
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void cb_ta_focused(lv_event_t *e) {
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, (lv_obj_t *)lv_event_get_target(e));
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void open_wifi_sta_dialog() {
    if (s_wifi_dialog) return;

    lv_obj_t *scr = lv_screen_active();
    // Dark semi-transparent overlay
    s_wifi_dialog = lv_obj_create(scr);
    lv_obj_set_size(s_wifi_dialog, BRL_SCREEN_W, BRL_SCREEN_H);
    lv_obj_set_pos(s_wifi_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_dialog, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_wifi_dialog, LV_OPA_80, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_wifi_dialog, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);

    // Dialog card
    lv_obj_t *card = lv_obj_create(s_wifi_dialog);
    lv_obj_set_size(card, 500, 320);
    lv_obj_center(card);
    brl_style_card(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(card, 16, LV_STATE_DEFAULT);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text(ttl, LV_SYMBOL_WIFI "  WLAN einrichten");
    brl_style_label(ttl, &lv_font_montserrat_20, BRL_CLR_TEXT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    // SSID field
    lv_obj_t *lbl_s = lv_label_create(card);
    lv_label_set_text(lbl_s, "Netzwerkname (SSID)");
    brl_style_label(lbl_s, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(lbl_s, LV_ALIGN_TOP_LEFT, 0, 36);

    s_ta_ssid = lv_textarea_create(card);
    lv_obj_set_size(s_ta_ssid, 468, 40);
    lv_obj_align(s_ta_ssid, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_textarea_set_one_line(s_ta_ssid, true);
    lv_textarea_set_placeholder_text(s_ta_ssid, "MeinHeimnetz");
    lv_obj_set_style_bg_color(s_ta_ssid, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_ta_ssid, BRL_CLR_TEXT, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_ta_ssid, cb_ta_focused, LV_EVENT_FOCUSED, nullptr);

    // Password field
    lv_obj_t *lbl_p = lv_label_create(card);
    lv_label_set_text(lbl_p, "Passwort");
    brl_style_label(lbl_p, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(lbl_p, LV_ALIGN_TOP_LEFT, 0, 104);

    s_ta_pass = lv_textarea_create(card);
    lv_obj_set_size(s_ta_pass, 468, 40);
    lv_obj_align(s_ta_pass, LV_ALIGN_TOP_LEFT, 0, 124);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "••••••••");
    lv_obj_set_style_bg_color(s_ta_pass, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_ta_pass, BRL_CLR_TEXT, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_ta_pass, cb_ta_focused, LV_EVENT_FOCUSED, nullptr);

    // Buttons
    lv_obj_t *btn_save = lv_button_create(card);
    lv_obj_set_size(btn_save, 140, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_save, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_save, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_save, 0, LV_STATE_DEFAULT);
    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, LV_SYMBOL_OK "  Verbinden");
    brl_style_label(lbl_save, &lv_font_montserrat_14, BRL_CLR_TEXT);
    lv_obj_center(lbl_save);
    lv_obj_add_event_cb(btn_save, cb_wifi_dialog_save, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_cancel = lv_button_create(card);
    lv_obj_set_size(btn_cancel, 120, 40);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -150, 0);
    lv_obj_set_style_bg_color(btn_cancel, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_cancel, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_cancel, 0, LV_STATE_DEFAULT);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, LV_SYMBOL_CLOSE "  Abbrechen");
    brl_style_label(lbl_cancel, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
    lv_obj_center(lbl_cancel);
    lv_obj_add_event_cb(btn_cancel, cb_wifi_dialog_cancel, LV_EVENT_CLICKED, nullptr);

    // Inline keyboard (hidden until textarea focused)
    s_kb = lv_keyboard_create(s_wifi_dialog);
    lv_obj_set_size(s_kb, BRL_SCREEN_W, 200);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_kb, cb_kb_ready, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Settings callbacks
// ---------------------------------------------------------------------------
static void cb_obd_connect(lv_event_t * /*e*/) {
    if (obd_bt_state() == OBD_CONNECTED || obd_bt_state() == OBD_REQUESTING) {
        obd_bt_disconnect();
    } else {
        // Force a new scan by resetting to IDLE — obd_bt_poll() does the rest
        obd_bt_disconnect();
    }
}

static void cb_wifi_ap_toggle(lv_event_t *e) {
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    wifi_set_mode(on ? BRL_WIFI_AP : BRL_WIFI_OFF);
}

static void cb_wifi_sta_setup(lv_event_t * /*e*/) {
    open_wifi_sta_dialog();
}

static void cb_wifi_sta_disconnect(lv_event_t * /*e*/) {
    wifi_set_mode(BRL_WIFI_OFF);
}

static void cb_lang_toggle(lv_event_t * /*e*/) {
    g_state.language = (g_state.language == 0) ? 1 : 0;
    lv_label_set_text(lv_obj_get_child(set_lang_btn, 0),
                      g_state.language == 0 ? "Deutsch" : "English");
}

static void cb_units_toggle(lv_event_t * /*e*/) {
    g_state.units = (g_state.units == 0) ? 1 : 0;
    lv_label_set_text(lv_obj_get_child(set_units_btn, 0),
                      g_state.units == 0 ? "km/h" : "mph");
}

// ---------------------------------------------------------------------------
// Build the settings panel
// ---------------------------------------------------------------------------
static void build_settings(lv_obj_t *parent) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Einstellungen");
    brl_style_label(title, &lv_font_montserrat_24, BRL_CLR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Scrollable container for rows
    lv_obj_t *scroll = lv_obj_create(parent);
    lv_obj_set_size(scroll, BRL_SCREEN_W - 16, BRL_CONTENT_H - 40);
    lv_obj_align(scroll, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(scroll, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(scroll, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(scroll, 4, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);

    // ── Row heights ──────────────────────────────────────────────────────────
    const int RH = 56;   // standard row height
    const int RH2 = 68;  // tall row (with subtitle)
    int y = 0;

    // ── 1. OBD Bluetooth ─────────────────────────────────────────────────────
    {
        lv_obj_t *right = make_setting_row(scroll, y, RH2,
            LV_SYMBOL_BLUETOOTH, "OBD Adapter",
            "BRL-OBD Bluetooth LE");
        y += RH2 + 4;

        set_obd_status_lbl = lv_label_create(right);
        lv_label_set_text(set_obd_status_lbl, "Nicht verbunden");
        brl_style_label(set_obd_status_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(set_obd_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        set_obd_btn = make_setting_btn(right, "Verbinden",
                                       BRL_CLR_ACCENT, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(set_obd_btn, cb_obd_connect, LV_EVENT_CLICKED, nullptr);
    }

    // ── 2. WiFi Hotspot (AP mode) ─────────────────────────────────────────────
    {
        lv_obj_t *right = make_setting_row(scroll, y, RH2,
            LV_SYMBOL_WIFI, "WiFi Hotspot (AP)",
            "Android App & Daten-Download");
        y += RH2 + 4;

        set_wifi_ap_status_lbl = lv_label_create(right);
        lv_label_set_text(set_wifi_ap_status_lbl, "AUS");
        brl_style_label(set_wifi_ap_status_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(set_wifi_ap_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        set_wifi_ap_sw = lv_switch_create(right);
        lv_obj_align(set_wifi_ap_sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(set_wifi_ap_sw, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(set_wifi_ap_sw, BRL_CLR_ACCENT, LV_STATE_CHECKED);
        lv_obj_add_event_cb(set_wifi_ap_sw, cb_wifi_ap_toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // ── 3. WiFi Netzwerk (STA / OTA) ─────────────────────────────────────────
    {
        lv_obj_t *right = make_setting_row(scroll, y, RH2,
            LV_SYMBOL_WIFI, "WiFi Netzwerk (STA)",
            "Für OTA Firmware Updates");
        y += RH2 + 4;

        set_wifi_sta_status_lbl = lv_label_create(right);
        lv_label_set_text(set_wifi_sta_status_lbl, "Nicht verbunden");
        brl_style_label(set_wifi_sta_status_lbl, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(set_wifi_sta_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *btn_cfg = make_setting_btn(right, LV_SYMBOL_SETTINGS " Einrichten",
                                              BRL_CLR_ACCENT, LV_ALIGN_RIGHT_MID, -130);
        lv_obj_add_event_cb(btn_cfg, cb_wifi_sta_setup, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *btn_dis = make_setting_btn(right, LV_SYMBOL_CLOSE " Trennen",
                                              BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(btn_dis, cb_wifi_sta_disconnect, LV_EVENT_CLICKED, nullptr);
    }

    // ── 4. Sprache ────────────────────────────────────────────────────────────
    {
        lv_obj_t *right = make_setting_row(scroll, y, RH, LV_SYMBOL_SETTINGS, "Sprache");
        y += RH + 4;

        set_lang_btn = make_setting_btn(right,
            g_state.language == 0 ? "Deutsch" : "English",
            BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(set_lang_btn, cb_lang_toggle, LV_EVENT_CLICKED, nullptr);
    }

    // ── 5. Einheiten ──────────────────────────────────────────────────────────
    {
        lv_obj_t *right = make_setting_row(scroll, y, RH, LV_SYMBOL_LOOP, "Einheiten");
        y += RH + 4;

        set_units_btn = make_setting_btn(right,
            g_state.units == 0 ? "km/h" : "mph",
            BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(set_units_btn, cb_units_toggle, LV_EVENT_CLICKED, nullptr);
    }

    // ── 6. Info ───────────────────────────────────────────────────────────────
    {
        lv_obj_t *right = make_setting_row(scroll, y, RH,
            LV_SYMBOL_GPS, "BRL Laptimer");
        (void)right;
        lv_obj_t *ver = lv_label_create(right);
        lv_label_set_text(ver, "v1.0.0 — Bavarian RaceLabs LLC");
        brl_style_label(ver, &lv_font_montserrat_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(ver, LV_ALIGN_RIGHT_MID, 0, 0);
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

    // --- Settings status labels ---
    if (set_obd_status_lbl) {
        OBdBtState os = obd_bt_state();
        const char *obd_txt  = "Nicht verbunden";
        const char *obd_btn  = "Verbinden";
        lv_color_t  obd_col  = BRL_CLR_TEXT_DIM;
        lv_color_t  btn_col  = BRL_CLR_ACCENT;
        if      (os == OBD_SCANNING || os == OBD_FOUND || os == OBD_CONNECTING) {
            obd_txt = "Suche..."; obd_col = BRL_CLR_WARN;
        } else if (os == OBD_CONNECTED || os == OBD_REQUESTING) {
            obd_txt = "Verbunden"; obd_col = BRL_CLR_ACCENT;
            obd_btn = "Trennen";   btn_col = BRL_CLR_DANGER;
        } else if (os == OBD_ERROR) {
            obd_txt = "Fehler — retry..."; obd_col = BRL_CLR_DANGER;
        }
        lv_label_set_text(set_obd_status_lbl, obd_txt);
        lv_obj_set_style_text_color(set_obd_status_lbl, obd_col, LV_STATE_DEFAULT);
        lv_label_set_text(lv_obj_get_child(set_obd_btn, 0), obd_btn);
        lv_obj_set_style_bg_color(set_obd_btn, btn_col, LV_STATE_DEFAULT);
    }

    if (set_wifi_ap_status_lbl) {
        bool ap_on = (g_state.wifi_mode == BRL_WIFI_AP);
        lv_label_set_text(set_wifi_ap_status_lbl,
            ap_on ? "AN  — BRL-Laptimer (192.168.4.1)" : "AUS");
        lv_obj_set_style_text_color(set_wifi_ap_status_lbl,
            ap_on ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);
        // Sync switch state without firing the event
        if (ap_on && !lv_obj_has_state(set_wifi_ap_sw, LV_STATE_CHECKED))
            lv_obj_add_state(set_wifi_ap_sw, LV_STATE_CHECKED);
        else if (!ap_on && lv_obj_has_state(set_wifi_ap_sw, LV_STATE_CHECKED))
            lv_obj_remove_state(set_wifi_ap_sw, LV_STATE_CHECKED);
    }

    if (set_wifi_sta_status_lbl) {
        bool sta_on = (g_state.wifi_mode == BRL_WIFI_STA || g_state.wifi_mode == BRL_WIFI_OTA);
        bool ota    = (g_state.wifi_mode == BRL_WIFI_OTA);
        lv_label_set_text(set_wifi_sta_status_lbl,
            ota    ? "OTA Update läuft..." :
            sta_on ? g_state.wifi_ssid     : "Nicht verbunden");
        lv_obj_set_style_text_color(set_wifi_sta_status_lbl,
            sta_on ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM, LV_STATE_DEFAULT);
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
