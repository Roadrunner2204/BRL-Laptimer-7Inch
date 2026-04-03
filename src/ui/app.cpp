/**
 * BRL Laptimer — Application UI
 *
 * Screen flow:
 *   Splash → Main Menu → [TIMING]     → Track Selection → Timing Screen
 *                      → [STRECKEN]   → Track Selection (+ Neue Strecke Wizard)
 *                      → [VERLAUF]    → History
 *                      → [EINSTELLUNGEN] → Settings
 */

#include <Arduino.h>
#include <math.h>
#include <lvgl.h>
#include "brl_fonts.h"
#include "../../include/i18n.h"
#include "app.h"
#include "theme.h"
#include "screen_splash.h"
#include "screen_timing.h"
#include "dash_config.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include "../timing/lap_timer.h"
#include "../obd/obd_bt.h"
#include "../wifi/wifi_mgr.h"
#include "../storage/session_store.h"
#include <SD.h>

// ---------------------------------------------------------------------------
// Global application state
// ---------------------------------------------------------------------------
AppState g_state = {};

// ---------------------------------------------------------------------------
// Status bar handles (one set per long-lived screen)
// ---------------------------------------------------------------------------
struct SbH { lv_obj_t *gps, *wifi, *obd; };
static SbH sb_menu = {};   // menu screen — cached, never rebuilt
static SbH sb_sub  = {};   // tracks / history / settings — cleared on back

// ---------------------------------------------------------------------------
// Screen pointers
// ---------------------------------------------------------------------------
static lv_obj_t *s_scr_menu  = nullptr;   // cached forever
static lv_obj_t *s_scr_sub   = nullptr;   // rebuilt each time

// ---------------------------------------------------------------------------
// Settings interactive handles (kept alive while settings screen exists)
// ---------------------------------------------------------------------------
static lv_obj_t *set_obd_status_lbl    = nullptr;
static lv_obj_t *set_obd_btn          = nullptr;
static lv_obj_t *set_wifi_ap_sw       = nullptr;
static lv_obj_t *set_wifi_ap_status_lbl = nullptr;
static lv_obj_t *set_wifi_sta_status_lbl = nullptr;
static lv_obj_t *set_lang_btn         = nullptr;
static lv_obj_t *set_units_btn        = nullptr;

// WiFi credential dialog (modal)
static lv_obj_t *s_wifi_dialog = nullptr;
static lv_obj_t *s_ta_ssid     = nullptr;
static lv_obj_t *s_ta_pass     = nullptr;
static lv_obj_t *s_dlg_kb      = nullptr;

// Track creator state
static lv_obj_t *s_tc_name     = nullptr;
static lv_obj_t *s_tc_sf1_lat  = nullptr, *s_tc_sf1_lon = nullptr;
static lv_obj_t *s_tc_sf2_lat  = nullptr, *s_tc_sf2_lon = nullptr;
static lv_obj_t *s_tc_fin_box  = nullptr;  // shown for A-B
static lv_obj_t *s_tc_fin1_lat = nullptr, *s_tc_fin1_lon = nullptr;
static lv_obj_t *s_tc_fin2_lat = nullptr, *s_tc_fin2_lon = nullptr;
static lv_obj_t *s_tc_kb       = nullptr;
static lv_obj_t *s_tc_type_circ_btn = nullptr;
static lv_obj_t *s_tc_type_ab_btn   = nullptr;
static bool      s_tc_is_circuit    = true;
static lv_obj_t *s_tc_sec_lat[MAX_SECTORS] = {};
static lv_obj_t *s_tc_sec_lon[MAX_SECTORS] = {};
static int       s_tc_sec_count = 0;
static lv_obj_t *s_tc_sec_container = nullptr;
static lv_obj_t *s_tc_add_sec_btn   = nullptr;
static int       s_tc_edit_idx      = -1;   // -1 = new, >=0 = editing existing track

// Track list filter
static char      s_filter_country[32] = {};  // empty = show all

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void open_tracks_screen();
static void open_track_creator(lv_obj_t *parent_scroll, int edit_idx = -1);
static void open_history_screen();
static void open_settings_screen();

// ============================================================================
// SECTION 1 — SHARED HELPERS
// ============================================================================

// Build a 40px status bar on any screen; fills *out with label handles
static void build_sb(lv_obj_t *scr, SbH *out) {
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 800, 40);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, BRL_CLR_STATUSBAR, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(bar, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    out->gps = lv_label_create(bar);
    lv_label_set_text(out->gps, LV_SYMBOL_GPS " 0");
    brl_style_label(out->gps, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(out->gps, 8, 12);

    out->wifi = lv_label_create(bar);
    lv_label_set_text(out->wifi, LV_SYMBOL_WIFI " --");
    brl_style_label(out->wifi, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(out->wifi, 140, 12);

    out->obd = lv_label_create(bar);
    lv_label_set_text(out->obd, LV_SYMBOL_BLUETOOTH " OBD --");
    brl_style_label(out->obd, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(out->obd, 270, 12);
}

// Build a sub-screen header bar (back + title) at y=40, h=50
static lv_obj_t *build_sub_header(lv_obj_t *scr, const char *title,
                                   lv_event_cb_t back_cb,
                                   lv_obj_t **action_btn_out = nullptr,
                                   const char *action_label = nullptr) {
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 800, 50);
    lv_obj_set_pos(hdr, 0, 40);
    lv_obj_set_style_bg_color(hdr, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_size(back, 110, 38);
    lv_obj_set_pos(back, 6, 6);
    brl_style_btn(back, BRL_CLR_SURFACE2);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text_fmt(bl, LV_SYMBOL_LEFT "  %s", tr(TR_MENU_BTN));
    brl_style_label(bl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, title);
    brl_style_label(ttl, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_align(ttl, LV_ALIGN_CENTER, 0, 0);

    if (action_btn_out && action_label) {
        lv_obj_t *abtn = lv_button_create(hdr);
        lv_obj_set_size(abtn, 140, 38);
        lv_obj_align(abtn, LV_ALIGN_RIGHT_MID, -6, 0);
        brl_style_btn(abtn, BRL_CLR_ACCENT);
        lv_obj_t *al = lv_label_create(abtn);
        lv_label_set_text(al, action_label);
        brl_style_label(al, &BRL_FONT_14, BRL_CLR_TEXT);
        lv_obj_center(al);
        *action_btn_out = abtn;
    }
    return hdr;
}

// Content area: y=90, h=390 (below status bar + header)
static lv_obj_t *build_content_area(lv_obj_t *scr, bool scrollable = true) {
    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_set_size(area, 800, 390);
    lv_obj_set_pos(area, 0, 90);
    lv_obj_set_style_bg_color(area, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(area, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(area, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(area, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(area, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(area, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(area, 8, LV_STATE_DEFAULT);
    if (!scrollable) lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);
    return area;
}

// Sub-screen skeleton: status bar + header + content area
static lv_obj_t *make_sub_screen(const char *title, lv_event_cb_t back_cb,
                                  lv_obj_t **action_btn = nullptr,
                                  const char *action_lbl = nullptr) {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_sb(scr, &sb_sub);
    build_sub_header(scr, title, back_cb, action_btn, action_lbl);
    return scr;
}

// ============================================================================
// SECTION 2 — NAVIGATION
// ============================================================================

void menu_screen_show() {
    sb_sub = {};   // clear sub-screen handles before possible deletion
    if (s_scr_sub) {
        lv_obj_delete(s_scr_sub);
        s_scr_sub = nullptr;
    }
    // Also clear settings handles (their objects were deleted above)
    set_obd_status_lbl = set_obd_btn = nullptr;
    set_wifi_ap_sw = set_wifi_ap_status_lbl = set_wifi_sta_status_lbl = nullptr;
    set_lang_btn = set_units_btn = nullptr;
    s_wifi_dialog = nullptr;
    s_tc_kb = nullptr;

    lv_screen_load(s_scr_menu);
}

static void cb_back_to_menu(lv_event_t * /*e*/) { menu_screen_show(); }

static void sub_screen_load(lv_obj_t *scr) {
    if (s_scr_sub && s_scr_sub != scr) {
        // Clear handles pointing into old screen before deleting
        sb_sub = {};
        set_obd_status_lbl = set_obd_btn = nullptr;
        set_wifi_ap_sw = set_wifi_ap_status_lbl = set_wifi_sta_status_lbl = nullptr;
        set_lang_btn = set_units_btn = nullptr;
        lv_obj_delete(s_scr_sub);
    }
    s_scr_sub = scr;
    lv_screen_load(scr);
}

// ============================================================================
// SECTION 3 — MAIN MENU SCREEN
// ============================================================================

static void cb_tile_timing(lv_event_t * /*e*/) {
    // Must pick a track first
    if (g_state.active_track_idx < 0) {
        open_tracks_screen();
    } else {
        timing_screen_open();
    }
}
static void cb_tile_tracks (lv_event_t * /*e*/) { open_tracks_screen();  }
static void cb_tile_history(lv_event_t * /*e*/) { open_history_screen(); }
static void cb_tile_settings(lv_event_t * /*e*/){ open_settings_screen();}

// is_rebuild=true: called after language change — don't load screen, don't duplicate timer
static void build_menu_screen(bool is_rebuild = false) {
    static bool s_timer_created = false;

    // If rebuilding, clean up old screen first
    if (s_scr_menu) {
        sb_menu = {};
        lv_obj_delete(s_scr_menu);
        s_scr_menu = nullptr;
    }

    s_scr_menu = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr_menu, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_scr_menu, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_scr_menu, LV_OBJ_FLAG_SCROLLABLE);

    build_sb(s_scr_menu, &sb_menu);

    // Title strip (y=40, h=48)
    lv_obj_t *strip = lv_obj_create(s_scr_menu);
    lv_obj_set_size(strip, 800, 48);
    lv_obj_set_pos(strip, 0, 40);
    lv_obj_set_style_bg_color(strip, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(strip, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(strip, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(strip, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(strip);
    lv_label_set_text(brand, "BAVARIAN RACELABS  —  BRL LAPTIMER");
    brl_style_label(brand, &BRL_FONT_16, BRL_CLR_ACCENT);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, 0);

    // 2×2 tile grid (y=96, available h=384)
    const int TW = 388, TH = 186, GAP = 8, X0 = 8, Y0 = 96;
    struct { const char *icon; TrKey label_key; TrKey sub_key; lv_event_cb_t cb; } tiles[4] = {
        { LV_SYMBOL_PLAY,     TR_TILE_TIMING,   TR_TILE_TIMING_SUB,   cb_tile_timing   },
        { LV_SYMBOL_GPS,      TR_TILE_TRACKS,   TR_TILE_TRACKS_SUB,   cb_tile_tracks   },
        { LV_SYMBOL_DRIVE,    TR_TILE_HISTORY,  TR_TILE_HISTORY_SUB,  cb_tile_history  },
        { LV_SYMBOL_SETTINGS, TR_TILE_SETTINGS, TR_TILE_SETTINGS_SUB, cb_tile_settings },
    };

    for (int i = 0; i < 4; i++) {
        int col = i % 2, row = i / 2;
        lv_obj_t *tile = lv_obj_create(s_scr_menu);
        lv_obj_set_size(tile, TW, TH);
        lv_obj_set_pos(tile, X0 + col*(TW+GAP), Y0 + row*(TH+GAP));
        brl_style_card(tile);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(tile, BRL_CLR_SURFACE2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(tile, tiles[i].cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *ico = lv_label_create(tile);
        lv_label_set_text(ico, tiles[i].icon);
        lv_obj_set_style_text_font(ico, &BRL_FONT_48, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ico, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
        lv_obj_align(ico, LV_ALIGN_CENTER, 0, -24);

        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, tr(tiles[i].label_key));
        brl_style_label(lbl, &BRL_FONT_20, BRL_CLR_TEXT);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -24);

        lv_obj_t *sub = lv_label_create(tile);
        lv_label_set_text(sub, tr(tiles[i].sub_key));
        brl_style_label(sub, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    if (!is_rebuild) {
        lv_screen_load(s_scr_menu);
    }
    if (!s_timer_created) {
        lv_timer_create([](lv_timer_t *t){
            extern void timer_live_update(lv_timer_t*);
            timer_live_update(t);
        }, 100, nullptr);
        s_timer_created = true;
    }
}

// ============================================================================
// SECTION 4 — TRACK SELECTION SCREEN
// ============================================================================

static double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0;
    double dlat = (lat2-lat1)*M_PI/180.0, dlon = (lon2-lon1)*M_PI/180.0;
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dlon/2)*sin(dlon/2);
    return R * 2.0 * atan2(sqrt(a), sqrt(1.0-a));
}

static void cb_track_select(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    g_state.active_track_idx = idx;
    lap_timer_set_track(idx);
    const TrackDef *td = track_get(idx);
    Serial.printf("[APP] Track selected: %s\n", td ? td->name : "?");
    timing_screen_open();
    // timing_screen_open loads a new LVGL screen — sub screen will be cleaned up on next menu_show
    s_scr_sub = nullptr;
}

static void cb_open_creator(lv_event_t *e) {
    lv_obj_t *scroll = (lv_obj_t*)lv_event_get_user_data(e);
    if (scroll) open_track_creator(scroll, -1);
}

static void cb_edit_track(lv_event_t *e) {
    lv_obj_t *scroll = (lv_obj_t*)lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    if (scroll) open_track_creator(scroll, idx);
}

static void cb_delete_track(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    if (idx < TRACK_DB_BUILTIN_COUNT) return;   // only user tracks
    int u_slot = idx - TRACK_DB_BUILTIN_COUNT;
    // Adjust active track pointer
    if (g_state.active_track_idx == idx)
        g_state.active_track_idx = -1;
    else if (g_state.active_track_idx > idx)
        g_state.active_track_idx--;
    session_store_delete_user_track(u_slot);
    open_tracks_screen();
}

static void open_tracks_screen() {
    int n = track_total_count();
    if (n > 60) n = 60;

    // ── Collect distances ──────────────────────────────────────────────────
    int sorted[60]; double dist[60];
    for (int i = 0; i < n; i++) {
        sorted[i] = i;
        const TrackDef *td = track_get(i);
        if (td && g_state.gps.valid) {
            double mlat = (td->sf_lat1 + td->sf_lat2) * 0.5;
            double mlon = (td->sf_lon1 + td->sf_lon2) * 0.5;
            dist[i] = haversine_km(g_state.gps.lat, g_state.gps.lon, mlat, mlon);
        } else dist[i] = 1e9;
    }
    // Sort by distance
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && dist[sorted[j]] < dist[sorted[j-1]]; j--)
            { int t=sorted[j]; sorted[j]=sorted[j-1]; sorted[j-1]=t; }

    // ── Collect unique countries ───────────────────────────────────────────
    char countries[16][32] = {};
    int  n_countries = 0;
    for (int s = 0; s < n; s++) {
        const TrackDef *td = track_get(sorted[s]);
        if (!td) continue;
        bool found = false;
        for (int c = 0; c < n_countries; c++)
            if (strcmp(countries[c], td->country) == 0) { found = true; break; }
        if (!found && n_countries < 16)
            strncpy(countries[n_countries++], td->country, 31);
    }

    // ── Build screen ──────────────────────────────────────────────────────
    lv_obj_t *new_btn = nullptr;
    char new_btn_label[48];
    snprintf(new_btn_label, sizeof(new_btn_label), LV_SYMBOL_PLUS " %s", tr(TR_NEW_TRACK));
    lv_obj_t *scr = make_sub_screen(tr(TR_SELECT_TRACK), cb_back_to_menu,
                                     &new_btn, new_btn_label);
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    // ── Country filter dropdown ───────────────────────────────────────────
    if (n_countries > 1) {
        // Build options string: "Alle\nDeutschland\nBelgien\n..."
        static char dd_opts[512];
        dd_opts[0] = '\0';
        strncpy(dd_opts, tr(TR_ALL_COUNTRIES), sizeof(dd_opts) - 1);
        int active_opt = 0;
        for (int c = 0; c < n_countries; c++) {
            strncat(dd_opts, "\n", sizeof(dd_opts) - strlen(dd_opts) - 1);
            strncat(dd_opts, countries[c], sizeof(dd_opts) - strlen(dd_opts) - 1);
            if (s_filter_country[0] != '\0' && strcmp(s_filter_country, countries[c]) == 0)
                active_opt = c + 1;
        }

        lv_obj_t *dd = lv_dropdown_create(content);
        lv_dropdown_set_options(dd, dd_opts);
        lv_dropdown_set_selected(dd, (uint32_t)active_opt);
        lv_obj_set_width(dd, 280);
        lv_obj_set_height(dd, 40);
        lv_obj_set_style_bg_color(dd, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(dd, BRL_CLR_TEXT, LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(dd, &BRL_FONT_16, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(dd, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(dd, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_hor(dd, 10, LV_STATE_DEFAULT);
        // Style the dropdown list
        lv_obj_t *ddlist = lv_dropdown_get_list(dd);
        if (ddlist) {
            lv_obj_set_style_bg_color(ddlist, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(ddlist, BRL_CLR_TEXT, LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(ddlist, &BRL_FONT_16, LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(ddlist, 0, LV_STATE_DEFAULT);
            lv_obj_set_style_shadow_width(ddlist, 0, LV_STATE_DEFAULT);
        }
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            lv_obj_t *obj = (lv_obj_t*)lv_event_get_target(e);
            uint32_t sel = lv_dropdown_get_selected(obj);
            if (sel == 0) {
                s_filter_country[0] = '\0';
            } else {
                lv_dropdown_get_selected_str(obj, s_filter_country,
                                             sizeof(s_filter_country));
            }
            open_tracks_screen();
        }, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    // ── GPS hint ──────────────────────────────────────────────────────────
    if (!g_state.gps.valid) {
        lv_obj_t *hint = lv_label_create(content);
        lv_label_set_text_fmt(hint, LV_SYMBOL_GPS "  %s", tr(TR_NO_GPS_HINT));
        brl_style_label(hint, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    }

    // ── Track rows ────────────────────────────────────────────────────────
    for (int s = 0; s < n; s++) {
        int idx = sorted[s];
        const TrackDef *td = track_get(idx);
        if (!td) continue;

        // Apply country filter
        if (s_filter_country[0] != '\0' && strcmp(td->country, s_filter_country) != 0)
            continue;

        bool is_user = (idx >= TRACK_DB_BUILTIN_COUNT);

        char label[96];
        if (dist[idx] < 1e8)
            snprintf(label, sizeof(label), ">  %s  (%.0f km)",
                     td->name, dist[idx]);
        else
            snprintf(label, sizeof(label), ">  %s  - %s",
                     td->name, td->country);

        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 52);
        lv_obj_set_style_bg_color(row,
            idx == g_state.active_track_idx ? BRL_CLR_BORDER : BRL_CLR_SURFACE,
            LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(row, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(row, 6, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(row, 0, LV_STATE_DEFAULT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Right-side buttons: edit [+ delete for user tracks]
        // Delete button (user tracks only)
        int right_buttons_w = 66;   // edit button width + gap
        if (is_user) right_buttons_w += 66;  // + delete button

        // Select button (fills remaining width)
        int sel_w = 784 - right_buttons_w;
        lv_obj_t *sel_btn = lv_button_create(row);
        lv_obj_set_size(sel_btn, sel_w, 52);
        lv_obj_set_pos(sel_btn, 0, 0);
        lv_obj_set_style_bg_color(sel_btn, lv_color_hex(0), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(sel_btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(sel_btn, BRL_CLR_SURFACE2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(sel_btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(sel_btn, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(sel_btn, 6, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(sel_btn, 0, LV_STATE_DEFAULT);
        lv_obj_t *sel_lbl = lv_label_create(sel_btn);
        lv_label_set_text(sel_lbl, label);
        brl_style_label(sel_lbl, &BRL_FONT_16, BRL_CLR_TEXT);
        lv_obj_align(sel_lbl, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_user_data(sel_btn, (void*)(intptr_t)idx);
        lv_obj_add_event_cb(sel_btn, cb_track_select, LV_EVENT_CLICKED, nullptr);

        // Edit button
        lv_obj_t *edit_btn = lv_button_create(row);
        lv_obj_set_size(edit_btn, 58, 44);
        lv_obj_set_pos(edit_btn, sel_w + 4, 4);
        lv_obj_set_style_bg_color(edit_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(edit_btn, BRL_CLR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(edit_btn, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(edit_btn, 6, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(edit_btn, 0, LV_STATE_DEFAULT);
        lv_obj_t *edit_lbl = lv_label_create(edit_btn);
        lv_label_set_text(edit_lbl, LV_SYMBOL_EDIT);
        brl_style_label(edit_lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_center(edit_lbl);
        lv_obj_set_user_data(edit_btn, (void*)(intptr_t)idx);
        lv_obj_add_event_cb(edit_btn, cb_edit_track, LV_EVENT_CLICKED, (void*)content);

        // Delete button (user tracks only)
        if (is_user) {
            lv_obj_t *del_btn = lv_button_create(row);
            lv_obj_set_size(del_btn, 58, 44);
            lv_obj_set_pos(del_btn, sel_w + 68, 4);
            lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x5A1A1A), LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(del_btn, BRL_CLR_DANGER, LV_STATE_PRESSED);
            lv_obj_set_style_border_width(del_btn, 0, LV_STATE_DEFAULT);
            lv_obj_set_style_radius(del_btn, 6, LV_STATE_DEFAULT);
            lv_obj_set_style_shadow_width(del_btn, 0, LV_STATE_DEFAULT);
            lv_obj_t *del_lbl = lv_label_create(del_btn);
            lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
            brl_style_label(del_lbl, &BRL_FONT_16, lv_color_hex(0xFF6666));
            lv_obj_center(del_lbl);
            lv_obj_set_user_data(del_btn, (void*)(intptr_t)idx);
            lv_obj_add_event_cb(del_btn, cb_delete_track, LV_EVENT_CLICKED, nullptr);
        }
    }

    if (new_btn) lv_obj_add_event_cb(new_btn, cb_open_creator, LV_EVENT_CLICKED, (void*)content);
    sub_screen_load(scr);
}

// ============================================================================
// SECTION 5 — TRACK CREATOR
// ============================================================================

static lv_obj_t *mk_coord_row(lv_obj_t *parent, const char *label,
                               lv_obj_t **ta_lat, lv_obj_t **ta_lon) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    brl_style_transparent(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, LV_STATE_DEFAULT);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    brl_style_label(lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_width(lbl, 80);
    lv_obj_set_style_pad_top(lbl, 10, LV_STATE_DEFAULT);

    *ta_lat = lv_textarea_create(row);
    lv_obj_set_width(*ta_lat, 160); lv_obj_set_height(*ta_lat, 38);
    lv_textarea_set_one_line(*ta_lat, true);
    lv_textarea_set_placeholder_text(*ta_lat, "Lat z.B. 50.3356");
    lv_obj_set_style_bg_color(*ta_lat, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(*ta_lat, BRL_CLR_TEXT, LV_STATE_DEFAULT);

    lv_obj_t *sep = lv_label_create(row);
    lv_label_set_text(sep, " / ");
    brl_style_label(sep, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_style_pad_top(sep, 10, LV_STATE_DEFAULT);

    *ta_lon = lv_textarea_create(row);
    lv_obj_set_width(*ta_lon, 160); lv_obj_set_height(*ta_lon, 38);
    lv_textarea_set_one_line(*ta_lon, true);
    lv_textarea_set_placeholder_text(*ta_lon, "Lon z.B. 6.9475");
    lv_obj_set_style_bg_color(*ta_lon, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(*ta_lon, BRL_CLR_TEXT, LV_STATE_DEFAULT);

    // Focus → show keyboard
    auto kb_focus = [](lv_event_t *ev){
        if (s_tc_kb) {
            lv_keyboard_set_textarea(s_tc_kb, (lv_obj_t*)lv_event_get_target(ev));
            lv_obj_remove_flag(s_tc_kb, LV_OBJ_FLAG_HIDDEN);
        }
    };
    lv_obj_add_event_cb(*ta_lat, kb_focus, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(*ta_lon, kb_focus, LV_EVENT_FOCUSED, nullptr);
    return row;
}

static lv_obj_t *mk_section_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    brl_style_label(lbl, &BRL_FONT_16, BRL_CLR_ACCENT);
    return lbl;
}

static void cb_tc_type_circuit(lv_event_t* /*e*/) {
    s_tc_is_circuit = true;
    if (s_tc_fin_box) lv_obj_add_flag(s_tc_fin_box, LV_OBJ_FLAG_HIDDEN);
    if (s_tc_type_circ_btn)
        lv_obj_set_style_bg_color(s_tc_type_circ_btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    if (s_tc_type_ab_btn)
        lv_obj_set_style_bg_color(s_tc_type_ab_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
}
static void cb_tc_type_ab(lv_event_t* /*e*/) {
    s_tc_is_circuit = false;
    if (s_tc_fin_box) lv_obj_remove_flag(s_tc_fin_box, LV_OBJ_FLAG_HIDDEN);
    if (s_tc_type_circ_btn)
        lv_obj_set_style_bg_color(s_tc_type_circ_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    if (s_tc_type_ab_btn)
        lv_obj_set_style_bg_color(s_tc_type_ab_btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
}

static void cb_tc_add_sector(lv_event_t* /*e*/) {
    if (s_tc_sec_count >= MAX_SECTORS || !s_tc_sec_container) return;
    char sec_lbl[8]; snprintf(sec_lbl, sizeof(sec_lbl), "S%d", s_tc_sec_count+1);
    mk_coord_row(s_tc_sec_container, sec_lbl,
                 &s_tc_sec_lat[s_tc_sec_count], &s_tc_sec_lon[s_tc_sec_count]);
    s_tc_sec_count++;
    if (s_tc_sec_count >= MAX_SECTORS && s_tc_add_sec_btn)
        lv_obj_add_flag(s_tc_add_sec_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_tc_kb) lv_obj_add_flag(s_tc_kb, LV_OBJ_FLAG_HIDDEN);
}

// Fill common coordinate fields from textarea handles into a TrackDef
static void tc_fill_coords(TrackDef &td) {
    td.sf_lat1 = atof(lv_textarea_get_text(s_tc_sf1_lat));
    td.sf_lon1 = atof(lv_textarea_get_text(s_tc_sf1_lon));
    td.sf_lat2 = atof(lv_textarea_get_text(s_tc_sf2_lat));
    td.sf_lon2 = atof(lv_textarea_get_text(s_tc_sf2_lon));
    if (!td.is_circuit && s_tc_fin1_lat) {
        td.fin_lat1 = atof(lv_textarea_get_text(s_tc_fin1_lat));
        td.fin_lon1 = atof(lv_textarea_get_text(s_tc_fin1_lon));
        td.fin_lat2 = atof(lv_textarea_get_text(s_tc_fin2_lat));
        td.fin_lon2 = atof(lv_textarea_get_text(s_tc_fin2_lon));
    }
    for (int i = 0; i < s_tc_sec_count && i < MAX_SECTORS; i++) {
        td.sectors[i].lat = atof(lv_textarea_get_text(s_tc_sec_lat[i]));
        td.sectors[i].lon = atof(lv_textarea_get_text(s_tc_sec_lon[i]));
        snprintf(td.sectors[i].name, SECTOR_NAME_LEN, "S%d", i+1);
    }
    td.sector_count = (uint8_t)s_tc_sec_count;
}

static void cb_tc_save(lv_event_t* /*e*/) {
    if (!s_tc_name || !s_tc_sf1_lat || !s_tc_sf1_lon || !s_tc_sf2_lat || !s_tc_sf2_lon) return;
    const char *name = lv_textarea_get_text(s_tc_name);
    if (!name || strlen(name) < 2) return;

    int save_idx = -1;

    // ── Case A: editing a built-in track → coordinate override ───────────
    if (s_tc_edit_idx >= 0 && s_tc_edit_idx < TRACK_DB_BUILTIN_COUNT) {
        // Start from original definition so name/country/length are preserved
        g_builtin_overrides[s_tc_edit_idx] = TRACK_DB[s_tc_edit_idx];
        TrackDef &td = g_builtin_overrides[s_tc_edit_idx];
        td.is_circuit = s_tc_is_circuit;
        tc_fill_coords(td);
        g_builtin_override_set[s_tc_edit_idx] = true;
        session_store_save_builtin_override(s_tc_edit_idx, &td);
        save_idx = s_tc_edit_idx;

    // ── Case B: new track or editing an existing user track ───────────────
    } else {
        int u_slot = -1;
        if (s_tc_edit_idx >= TRACK_DB_BUILTIN_COUNT)
            u_slot = s_tc_edit_idx - TRACK_DB_BUILTIN_COUNT;
        if (u_slot < 0) {
            if (g_user_track_count >= MAX_USER_TRACKS) return;
            u_slot = g_user_track_count;
        }
        TrackDef &td = g_user_tracks[u_slot];
        memset(&td, 0, sizeof(td));
        strncpy(td.name, name, sizeof(td.name)-1);
        strncpy(td.country, tr(TR_CUSTOM_COUNTRY), sizeof(td.country)-1);
        td.is_circuit   = s_tc_is_circuit;
        td.user_created = true;
        tc_fill_coords(td);
        if (u_slot == g_user_track_count) g_user_track_count++;
        session_store_save_user_track(&td);
        save_idx = TRACK_DB_BUILTIN_COUNT + u_slot;
    }

    g_state.active_track_idx = save_idx;
    lap_timer_set_track(save_idx);
    s_tc_edit_idx = -1;
    timing_screen_open();
    s_scr_sub = nullptr;
}

static void open_track_creator(lv_obj_t *scroll, int edit_idx) {
    // Reset creator state
    s_tc_edit_idx = edit_idx;
    s_tc_name = s_tc_sf1_lat = s_tc_sf1_lon = s_tc_sf2_lat = s_tc_sf2_lon = nullptr;
    s_tc_fin1_lat = s_tc_fin1_lon = s_tc_fin2_lat = s_tc_fin2_lon = nullptr;
    s_tc_fin_box = s_tc_sec_container = s_tc_add_sec_btn = nullptr;
    memset(s_tc_sec_lat, 0, sizeof(s_tc_sec_lat));
    memset(s_tc_sec_lon, 0, sizeof(s_tc_sec_lon));
    s_tc_sec_count = 0;
    s_tc_is_circuit = true;
    const TrackDef *edit_td = (edit_idx >= 0) ? track_get(edit_idx) : nullptr;
    if (edit_td) s_tc_is_circuit = edit_td->is_circuit;

    // Clear the existing list and build creator form in place
    lv_obj_clean(scroll);

    auto section = [&](const char *t){ mk_section_label(scroll, t); };
    auto kb_focus_name = [](lv_event_t *ev){
        if (s_tc_kb) {
            lv_keyboard_set_textarea(s_tc_kb, (lv_obj_t*)lv_event_get_target(ev));
            lv_obj_remove_flag(s_tc_kb, LV_OBJ_FLAG_HIDDEN);
        }
    };

    // Name
    section(tr(TR_SEC_NAME));
    s_tc_name = lv_textarea_create(scroll);
    lv_obj_set_width(s_tc_name, 500); lv_obj_set_height(s_tc_name, 40);
    lv_textarea_set_one_line(s_tc_name, true);
    lv_textarea_set_placeholder_text(s_tc_name, tr(TR_NAME_HINT));
    lv_obj_set_style_bg_color(s_tc_name, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_tc_name, BRL_CLR_TEXT, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_tc_name, kb_focus_name, LV_EVENT_FOCUSED, nullptr);

    // Type selector
    section(tr(TR_SEC_TRACKTYPE));
    lv_obj_t *type_row = lv_obj_create(scroll);
    lv_obj_set_width(type_row, LV_PCT(100)); lv_obj_set_height(type_row, LV_SIZE_CONTENT);
    brl_style_transparent(type_row);
    lv_obj_remove_flag(type_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(type_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(type_row, 8, LV_STATE_DEFAULT);

    s_tc_type_circ_btn = lv_button_create(type_row);
    lv_obj_set_size(s_tc_type_circ_btn, 200, 44);
    lv_obj_set_style_bg_color(s_tc_type_circ_btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_tc_type_circ_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_tc_type_circ_btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *lc = lv_label_create(s_tc_type_circ_btn);
    lv_label_set_text_fmt(lc, LV_SYMBOL_LOOP "  %s", tr(TR_CIRCUIT_BTN));
    brl_style_label(lc, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_center(lc);
    lv_obj_add_event_cb(s_tc_type_circ_btn, cb_tc_type_circuit, LV_EVENT_CLICKED, nullptr);

    s_tc_type_ab_btn = lv_button_create(type_row);
    lv_obj_set_size(s_tc_type_ab_btn, 200, 44);
    lv_obj_set_style_bg_color(s_tc_type_ab_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_tc_type_ab_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_tc_type_ab_btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *lab = lv_label_create(s_tc_type_ab_btn);
    lv_label_set_text_fmt(lab, LV_SYMBOL_RIGHT "  %s", tr(TR_AB_BTN));
    brl_style_label(lab, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_center(lab);
    lv_obj_add_event_cb(s_tc_type_ab_btn, cb_tc_type_ab, LV_EVENT_CLICKED, nullptr);

    // S/F line
    section(tr(TR_SEC_SF));
    mk_coord_row(scroll, "Punkt 1:", &s_tc_sf1_lat, &s_tc_sf1_lon);
    mk_coord_row(scroll, "Punkt 2:", &s_tc_sf2_lat, &s_tc_sf2_lon);

    // A-B finish section (hidden by default)
    s_tc_fin_box = lv_obj_create(scroll);
    lv_obj_set_width(s_tc_fin_box, LV_PCT(100)); lv_obj_set_height(s_tc_fin_box, LV_SIZE_CONTENT);
    brl_style_transparent(s_tc_fin_box);
    lv_obj_remove_flag(s_tc_fin_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_tc_fin_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_tc_fin_box, LV_OBJ_FLAG_HIDDEN);
    mk_section_label(s_tc_fin_box, tr(TR_SEC_FINISH));
    mk_coord_row(s_tc_fin_box, "Punkt 1:", &s_tc_fin1_lat, &s_tc_fin1_lon);
    mk_coord_row(s_tc_fin_box, "Punkt 2:", &s_tc_fin2_lat, &s_tc_fin2_lon);

    // Sectors
    section(tr(TR_SEC_SECTORS));
    s_tc_sec_container = lv_obj_create(scroll);
    lv_obj_set_width(s_tc_sec_container, LV_PCT(100));
    lv_obj_set_height(s_tc_sec_container, LV_SIZE_CONTENT);
    brl_style_transparent(s_tc_sec_container);
    lv_obj_remove_flag(s_tc_sec_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_tc_sec_container, LV_FLEX_FLOW_COLUMN);

    s_tc_add_sec_btn = lv_button_create(scroll);
    lv_obj_set_size(s_tc_add_sec_btn, 220, 40);
    lv_obj_set_style_bg_color(s_tc_add_sec_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_tc_add_sec_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_tc_add_sec_btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *asl = lv_label_create(s_tc_add_sec_btn);
    lv_label_set_text_fmt(asl, LV_SYMBOL_PLUS "  %s", tr(TR_ADD_SECTOR));
    brl_style_label(asl, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(asl);
    lv_obj_add_event_cb(s_tc_add_sec_btn, cb_tc_add_sector, LV_EVENT_CLICKED, nullptr);

    // Save button
    lv_obj_t *save_btn = lv_button_create(scroll);
    lv_obj_set_size(save_btn, 280, 50);
    lv_obj_set_style_bg_color(save_btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(save_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(save_btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *sl = lv_label_create(save_btn);
    lv_label_set_text_fmt(sl, LV_SYMBOL_OK "  %s", tr(TR_SAVE_START_TIMING));
    brl_style_label(sl, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_center(sl);
    lv_obj_add_event_cb(save_btn, cb_tc_save, LV_EVENT_CLICKED, nullptr);

    // Touch keyboard (shown when textarea focused)
    s_tc_kb = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(s_tc_kb, 800, 200);
    lv_obj_align(s_tc_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_tc_kb, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_tc_kb, [](lv_event_t *ev){
        lv_obj_add_flag((lv_obj_t*)lv_event_get_target(ev), LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(s_tc_kb, LV_OBJ_FLAG_HIDDEN);

    // Pre-fill fields when editing an existing track
    if (edit_td) {
        lv_textarea_set_text(s_tc_name, edit_td->name);

        // Circuit/A-B button state
        if (!edit_td->is_circuit) {
            lv_obj_set_style_bg_color(s_tc_type_circ_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(s_tc_type_ab_btn,   BRL_CLR_ACCENT,   LV_STATE_DEFAULT);
            if (s_tc_fin_box) lv_obj_remove_flag(s_tc_fin_box, LV_OBJ_FLAG_HIDDEN);
        }

        // S/F coordinates
        char buf[24];
        snprintf(buf, sizeof(buf), "%.7f", edit_td->sf_lat1); lv_textarea_set_text(s_tc_sf1_lat, buf);
        snprintf(buf, sizeof(buf), "%.7f", edit_td->sf_lon1); lv_textarea_set_text(s_tc_sf1_lon, buf);
        snprintf(buf, sizeof(buf), "%.7f", edit_td->sf_lat2); lv_textarea_set_text(s_tc_sf2_lat, buf);
        snprintf(buf, sizeof(buf), "%.7f", edit_td->sf_lon2); lv_textarea_set_text(s_tc_sf2_lon, buf);

        // Finish line (A-B)
        if (!edit_td->is_circuit && s_tc_fin1_lat) {
            snprintf(buf, sizeof(buf), "%.7f", edit_td->fin_lat1); lv_textarea_set_text(s_tc_fin1_lat, buf);
            snprintf(buf, sizeof(buf), "%.7f", edit_td->fin_lon1); lv_textarea_set_text(s_tc_fin1_lon, buf);
            snprintf(buf, sizeof(buf), "%.7f", edit_td->fin_lat2); lv_textarea_set_text(s_tc_fin2_lat, buf);
            snprintf(buf, sizeof(buf), "%.7f", edit_td->fin_lon2); lv_textarea_set_text(s_tc_fin2_lon, buf);
        }

        // Sectors
        for (int i = 0; i < edit_td->sector_count && i < MAX_SECTORS; i++) {
            char sec_lbl[8]; snprintf(sec_lbl, sizeof(sec_lbl), "S%d", i+1);
            mk_coord_row(s_tc_sec_container, sec_lbl,
                         &s_tc_sec_lat[i], &s_tc_sec_lon[i]);
            snprintf(buf, sizeof(buf), "%.7f", edit_td->sectors[i].lat);
            lv_textarea_set_text(s_tc_sec_lat[i], buf);
            snprintf(buf, sizeof(buf), "%.7f", edit_td->sectors[i].lon);
            lv_textarea_set_text(s_tc_sec_lon[i], buf);
            s_tc_sec_count = i + 1;
        }
        if (s_tc_sec_count >= MAX_SECTORS && s_tc_add_sec_btn)
            lv_obj_add_flag(s_tc_add_sec_btn, LV_OBJ_FLAG_HIDDEN);
    }
}


// ============================================================================
// SECTION 6 — HISTORY SCREEN
// ============================================================================

// Helper: section header label inside content area
static void hist_section_header(lv_obj_t *parent, const char *text) {
    lv_obj_t *hdr = lv_label_create(parent);
    lv_label_set_text(hdr, text);
    brl_style_label(hdr, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_style_pad_top(hdr, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(hdr, 2, LV_STATE_DEFAULT);
}

static void open_history_screen() {
    lv_obj_t *scr = make_sub_screen(tr(TR_HISTORY_TITLE), cb_back_to_menu);
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    // ── Current session ───────────────────────────────────────────────────
    hist_section_header(content, tr(TR_HIST_CURRENT));

    LapSession &sess = g_state.session;
    if (sess.lap_count == 0) {
        lv_obj_t *ph = lv_label_create(content);
        lv_label_set_text(ph, tr(TR_NO_LAPS));
        brl_style_label(ph, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    } else {
        for (int i = 0; i < sess.lap_count; i++) {
            RecordedLap &rl = sess.laps[i];
            lv_obj_t *row = lv_obj_create(content);
            lv_obj_set_width(row, LV_PCT(100)); lv_obj_set_height(row, 52);
            brl_style_card(row);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            char lt_buf[16]; fmt_laptime(lt_buf, sizeof(lt_buf), rl.total_ms);
            char lap_buf[64];
            snprintf(lap_buf, sizeof(lap_buf), "%s %d    %s%s",
                     tr(TR_LAP), i+1, lt_buf,
                     i == (int)sess.best_lap_idx ? "  * BEST" : "");

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, lap_buf);
            brl_style_label(lbl, &BRL_FONT_16,
                i == (int)sess.best_lap_idx ? BRL_CLR_ACCENT : BRL_CLR_TEXT);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

            // Ref lap button
            lv_obj_t *ref_btn = lv_button_create(row);
            lv_obj_set_size(ref_btn, 130, 34);
            lv_obj_align(ref_btn, LV_ALIGN_RIGHT_MID, 0, 0);
            brl_style_btn(ref_btn,
                i == (int)sess.ref_lap_idx ? BRL_CLR_ACCENT : BRL_CLR_SURFACE2);
            lv_obj_t *rl2 = lv_label_create(ref_btn);
            lv_label_set_text(rl2, i == (int)sess.ref_lap_idx ? tr(TR_IS_REF) : tr(TR_SET_REF));
            brl_style_label(rl2, &BRL_FONT_14, BRL_CLR_TEXT);
            lv_obj_center(rl2);
            lv_obj_set_user_data(ref_btn, (void*)(intptr_t)i);
            lv_obj_add_event_cb(ref_btn, [](lv_event_t *e){
                int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
                lap_timer_set_ref_lap((uint8_t)idx);
                open_history_screen();
            }, LV_EVENT_CLICKED, nullptr);
        }
    }

    // ── Saved sessions from SD ────────────────────────────────────────────
    hist_section_header(content, tr(TR_HIST_SAVED));

    if (!g_state.sd_available) {
        lv_obj_t *ph = lv_label_create(content);
        lv_label_set_text(ph, tr(TR_STORAGE_UNAVAIL));
        brl_style_label(ph, &BRL_FONT_14, BRL_CLR_DANGER);
    } else {
        static SessionSummary s_summaries[30];
        int n = session_store_list_summaries(s_summaries, 30);

        // Filter out current session
        int shown = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(s_summaries[i].id, sess.session_id) == 0) continue;

            lv_obj_t *row = lv_obj_create(content);
            lv_obj_set_width(row, LV_PCT(100)); lv_obj_set_height(row, 60);
            brl_style_card(row);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            // Track name + session id (small)
            lv_obj_t *track_lbl = lv_label_create(row);
            const char *tname = strlen(s_summaries[i].track) > 0
                                ? s_summaries[i].track : "?";
            lv_label_set_text(track_lbl, tname);
            brl_style_label(track_lbl, &BRL_FONT_16, BRL_CLR_TEXT);
            lv_obj_align(track_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

            // Session ID as subtitle
            lv_obj_t *id_lbl = lv_label_create(row);
            lv_label_set_text(id_lbl, s_summaries[i].id);
            brl_style_label(id_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
            lv_obj_align(id_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

            // Right side: N laps + best lap
            char info[48] = {};
            if (s_summaries[i].best_ms > 0) {
                char bt[16]; fmt_laptime(bt, sizeof(bt), s_summaries[i].best_ms);
                snprintf(info, sizeof(info), "%u %s  |  %s %s",
                         s_summaries[i].lap_count, tr(TR_HIST_LAPS),
                         tr(TR_HIST_BEST), bt);
            } else {
                snprintf(info, sizeof(info), "%u %s",
                         s_summaries[i].lap_count, tr(TR_HIST_LAPS));
            }
            lv_obj_t *info_lbl = lv_label_create(row);
            lv_label_set_text(info_lbl, info);
            brl_style_label(info_lbl, &BRL_FONT_14, BRL_CLR_ACCENT);
            lv_obj_align(info_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

            shown++;
        }
        if (shown == 0) {
            lv_obj_t *ph = lv_label_create(content);
            lv_label_set_text(ph, tr(TR_HIST_NO_SAVED));
            brl_style_label(ph, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        }
    }

    sub_screen_load(scr);
}

// ============================================================================
// SECTION 7 — SETTINGS SCREEN
// ============================================================================

static lv_obj_t *make_setting_row(lv_obj_t *parent, int /*y*/, int h,
                                   const char *icon, const char *title,
                                   const char *subtitle = nullptr) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, h);
    brl_style_card(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ico = lv_label_create(row);
    lv_label_set_text(ico, icon);
    brl_style_label(ico, &BRL_FONT_20, BRL_CLR_ACCENT);
    lv_obj_align(ico, LV_ALIGN_LEFT_MID, 0, subtitle ? -8 : 0);

    lv_obj_t *tit = lv_label_create(row);
    lv_label_set_text(tit, title);
    brl_style_label(tit, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_align(tit, LV_ALIGN_LEFT_MID, 32, subtitle ? -8 : 0);

    if (subtitle) {
        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text(sub, subtitle);
        brl_style_label(sub, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 32, 10);
    }

    lv_obj_t *right = lv_obj_create(row);
    lv_obj_set_size(right, 340, h - 16);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    brl_style_transparent(right);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    return right;
}

static lv_obj_t *make_setting_btn(lv_obj_t *parent, const char *text,
                                   lv_color_t color, lv_align_t align, int x_ofs = 0) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 120, 36);
    lv_obj_align(btn, align, x_ofs, 0);
    lv_obj_set_style_bg_color(btn, color, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    brl_style_label(lbl, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(lbl);
    return btn;
}

// WiFi dialog
static void cb_wifi_dialog_save(lv_event_t* /*e*/) {
    if (s_ta_ssid && s_ta_pass) {
        wifi_set_sta(lv_textarea_get_text(s_ta_ssid), lv_textarea_get_text(s_ta_pass));
        wifi_set_mode(BRL_WIFI_STA);
    }
    if (s_wifi_dialog) { lv_obj_delete(s_wifi_dialog); s_wifi_dialog = nullptr; }
}
static void cb_wifi_dialog_cancel(lv_event_t* /*e*/) {
    if (s_wifi_dialog) { lv_obj_delete(s_wifi_dialog); s_wifi_dialog = nullptr; }
}
static void cb_dlg_kb_ready(lv_event_t *e) {
    lv_obj_add_flag((lv_obj_t*)lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
}
static void cb_dlg_ta_focus(lv_event_t *e) {
    if (s_dlg_kb) {
        lv_keyboard_set_textarea(s_dlg_kb, (lv_obj_t*)lv_event_get_target(e));
        lv_obj_remove_flag(s_dlg_kb, LV_OBJ_FLAG_HIDDEN);
    }
}
static void open_wifi_sta_dialog() {
    if (s_wifi_dialog) return;
    lv_obj_t *scr = lv_screen_active();
    s_wifi_dialog = lv_obj_create(scr);
    lv_obj_set_size(s_wifi_dialog, 800, 480); lv_obj_set_pos(s_wifi_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_dialog, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_wifi_dialog, LV_OPA_80, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_wifi_dialog, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_wifi_dialog);
    lv_obj_set_size(card, 500, 300); lv_obj_center(card);
    brl_style_card(card); lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(card, 16, LV_STATE_DEFAULT);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text_fmt(ttl, LV_SYMBOL_WIFI "  %s", tr(TR_WIFI_DLG_TITLE));
    brl_style_label(ttl, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    auto mk_ta = [&](const char *hint, bool pw, int y) -> lv_obj_t* {
        lv_obj_t *ta = lv_textarea_create(card);
        lv_obj_set_size(ta, 468, 40); lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, y);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, hint);
        if (pw) lv_textarea_set_password_mode(ta, true);
        lv_obj_set_style_bg_color(ta, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ta, BRL_CLR_TEXT, LV_STATE_DEFAULT);
        lv_obj_add_event_cb(ta, cb_dlg_ta_focus, LV_EVENT_FOCUSED, nullptr);
        return ta;
    };
    lv_obj_t *l1 = lv_label_create(card);
    lv_label_set_text(l1, tr(TR_SSID_LABEL));
    brl_style_label(l1, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 36);
    s_ta_ssid = mk_ta("MeinHeimnetz", false, 56);

    lv_obj_t *l2 = lv_label_create(card);
    lv_label_set_text(l2, tr(TR_PASS_LABEL));
    brl_style_label(l2, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 106);
    s_ta_pass = mk_ta("••••••••", true, 126);

    auto mk_btn = [&](const char *t, lv_color_t c, int x) -> lv_obj_t* {
        lv_obj_t *b = lv_button_create(card);
        lv_obj_set_size(b, 140, 40); lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, x, 0);
        lv_obj_set_style_bg_color(b, c, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(b, 6, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(b, 0, LV_STATE_DEFAULT);
        lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, t);
        brl_style_label(l, &BRL_FONT_14, BRL_CLR_TEXT); lv_obj_center(l);
        return b;
    };
    char dlg_conn[48], dlg_cancel[48];
    snprintf(dlg_conn,   sizeof(dlg_conn),   LV_SYMBOL_OK    "  %s", tr(TR_CONNECT_DLG));
    snprintf(dlg_cancel, sizeof(dlg_cancel), LV_SYMBOL_CLOSE "  %s", tr(TR_CANCEL_DLG));
    lv_obj_t *bsave = mk_btn(dlg_conn, BRL_CLR_ACCENT, 0);
    lv_obj_add_event_cb(bsave, cb_wifi_dialog_save, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bcancel = mk_btn(dlg_cancel, BRL_CLR_SURFACE2, -150);
    lv_obj_add_event_cb(bcancel, cb_wifi_dialog_cancel, LV_EVENT_CLICKED, nullptr);

    s_dlg_kb = lv_keyboard_create(s_wifi_dialog);
    lv_obj_set_size(s_dlg_kb, 800, 200); lv_obj_align(s_dlg_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_dlg_kb, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_dlg_kb, cb_dlg_kb_ready, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(s_dlg_kb, LV_OBJ_FLAG_HIDDEN);
}

static void open_settings_screen() {
    lv_obj_t *scr = make_sub_screen(tr(TR_SETTINGS_TITLE), cb_back_to_menu);
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    const int RH = 56, RH2 = 68;

    // OBD
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_BLUETOOTH,
                                        "OBD Adapter", tr(TR_OBD_SUB));
        set_obd_status_lbl = lv_label_create(r);
        lv_label_set_text(set_obd_status_lbl, tr(TR_NOT_CONNECTED));
        brl_style_label(set_obd_status_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(set_obd_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);
        set_obd_btn = make_setting_btn(r, tr(TR_CONNECT_BTN), BRL_CLR_ACCENT, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(set_obd_btn, [](lv_event_t* /*e*/){
            if (obd_bt_state() == OBD_CONNECTED || obd_bt_state() == OBD_REQUESTING)
                obd_bt_disconnect();
            else obd_bt_disconnect(); // resets to IDLE → poll restarts scan
        }, LV_EVENT_CLICKED, nullptr);
    }
    // WiFi AP
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_WIFI,
                                        tr(TR_WIFI_AP_TITLE), tr(TR_WIFI_AP_SUB));
        set_wifi_ap_status_lbl = lv_label_create(r);
        lv_label_set_text(set_wifi_ap_status_lbl, tr(TR_WIFI_AP_OFF));
        brl_style_label(set_wifi_ap_status_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(set_wifi_ap_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);
        set_wifi_ap_sw = lv_switch_create(r);
        lv_obj_align(set_wifi_ap_sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(set_wifi_ap_sw, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(set_wifi_ap_sw, BRL_CLR_ACCENT, LV_STATE_CHECKED);
        lv_obj_add_event_cb(set_wifi_ap_sw, [](lv_event_t *e){
            wifi_set_mode(lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED)
                          ? BRL_WIFI_AP : BRL_WIFI_OFF);
        }, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    // WiFi STA
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_WIFI,
                                        tr(TR_WIFI_STA_TITLE), tr(TR_WIFI_STA_SUB));
        set_wifi_sta_status_lbl = lv_label_create(r);
        lv_label_set_text(set_wifi_sta_status_lbl, tr(TR_NOT_CONNECTED));
        brl_style_label(set_wifi_sta_status_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(set_wifi_sta_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);
        char cfg_lbl[48];
        snprintf(cfg_lbl, sizeof(cfg_lbl), LV_SYMBOL_SETTINGS " %s", tr(TR_CONFIGURE_BTN));
        lv_obj_t *bcfg = make_setting_btn(r, cfg_lbl, BRL_CLR_ACCENT, LV_ALIGN_RIGHT_MID, -130);
        lv_obj_add_event_cb(bcfg, [](lv_event_t* /*e*/){ open_wifi_sta_dialog(); },
                            LV_EVENT_CLICKED, nullptr);
        char dis_lbl[48];
        snprintf(dis_lbl, sizeof(dis_lbl), LV_SYMBOL_CLOSE " %s", tr(TR_DISCONNECT_BTN));
        lv_obj_t *bdis = make_setting_btn(r, dis_lbl, BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(bdis, [](lv_event_t* /*e*/){ wifi_set_mode(BRL_WIFI_OFF); },
                            LV_EVENT_CLICKED, nullptr);
    }
    // Language
    {
        lv_obj_t *r = make_setting_row(content, 0, RH, LV_SYMBOL_SETTINGS,
                                        tr(TR_LANGUAGE_LABEL));
        set_lang_btn = make_setting_btn(r,
            g_state.language == 0 ? "Deutsch" : "English",
            BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(set_lang_btn, [](lv_event_t* /*e*/){
            g_state.language ^= 1;
            g_dash_cfg.language = g_state.language;
            dash_config_save();
            i18n_set_language(g_state.language);
            lv_label_set_text(lv_obj_get_child(set_lang_btn, 0),
                              g_state.language == 0 ? "Deutsch" : "English");
            // Rebuild menu in background so it shows the new language on next visit
            build_menu_screen(true);
        }, LV_EVENT_CLICKED, nullptr);
    }
    // Units
    {
        lv_obj_t *r = make_setting_row(content, 0, RH, LV_SYMBOL_LOOP,
                                        tr(TR_UNITS_LABEL));
        set_units_btn = make_setting_btn(r, g_state.units == 0 ? "km/h" : "mph",
                                         BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(set_units_btn, [](lv_event_t* /*e*/){
            g_state.units ^= 1;
            g_dash_cfg.units = g_state.units;
            dash_config_save();
            lv_label_set_text(lv_obj_get_child(set_units_btn, 0),
                              g_state.units == 0 ? "km/h" : "mph");
        }, LV_EVENT_CLICKED, nullptr);
    }
    // Storage (SD card / Festplatte)
    {
        lv_obj_t *r = make_setting_row(content, 0, RH, LV_SYMBOL_SAVE,
                                        tr(TR_STORAGE_TITLE));
        lv_obj_t *stlbl = lv_label_create(r);
        if (!g_state.sd_available) {
            lv_label_set_text(stlbl, tr(TR_STORAGE_UNAVAIL));
            brl_style_label(stlbl, &BRL_FONT_14, BRL_CLR_DANGER);
        } else {
            uint64_t total_b = SD.totalBytes();
            uint64_t used_b  = SD.usedBytes();
            uint64_t free_b  = total_b - used_b;
            float total_gb   = (float)total_b / 1073741824.0f;
            float free_gb    = (float)free_b  / 1073741824.0f;
            int pct_used     = (total_b > 0)
                               ? (int)((float)used_b / (float)total_b * 100.0f) : 0;
            char sbuf[64];
            snprintf(sbuf, sizeof(sbuf), "%.1f GB %s / %.1f GB",
                     free_gb, tr(TR_STORAGE_USED), total_gb);
            lv_label_set_text(stlbl, sbuf);
            brl_style_label(stlbl, &BRL_FONT_14,
                            pct_used > 90 ? BRL_CLR_DANGER :
                            pct_used > 70 ? BRL_CLR_WARN : BRL_CLR_TEXT_DIM);
        }
        lv_obj_align(stlbl, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    // Info
    {
        lv_obj_t *r = make_setting_row(content, 0, RH, LV_SYMBOL_GPS, "BRL Laptimer");
        lv_obj_t *ver = lv_label_create(r);
        lv_label_set_text(ver, "v1.0.0 - Bavarian RaceLabs LLC");
        brl_style_label(ver, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        lv_obj_align(ver, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    sub_screen_load(scr);
}


// ---------------------------------------------------------------------------
// Section 8: Live update timer (100 ms)
// ---------------------------------------------------------------------------

// Helper: is OBD currently connected/requesting?
static inline bool obd_is_connected() {
    OBdBtState st = obd_bt_state();
    return st == OBD_CONNECTED || st == OBD_REQUESTING;
}
static inline bool obd_is_scanning() {
    OBdBtState st = obd_bt_state();
    return st == OBD_SCANNING || st == OBD_FOUND || st == OBD_CONNECTING;
}

static void update_sb(SbH &sb) {
    if (!sb.gps && !sb.wifi && !sb.obd) return;

    // GPS label
    if (sb.gps) {
        char buf[16];
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %d", (int)g_state.gps.satellites);
        lv_label_set_text(sb.gps, buf);
        lv_obj_set_style_text_color(sb.gps,
            g_state.gps.valid ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
    }
    // WiFi label
    if (sb.wifi) {
        const char *wlbl = "WiFi: OFF";
        lv_color_t wcol = lv_color_hex(0xAAAAAA);
        switch (g_state.wifi_mode) {
            case BRL_WIFI_AP:  wlbl = LV_SYMBOL_WIFI " AP";   wcol = lv_color_hex(0x00AAFF); break;
            case BRL_WIFI_STA: wlbl = LV_SYMBOL_WIFI " STA";  wcol = lv_color_hex(0x00CC66); break;
            case BRL_WIFI_OTA: wlbl = LV_SYMBOL_WIFI " OTA";  wcol = lv_color_hex(0xFFAA00); break;
            default: break;
        }
        lv_label_set_text(sb.wifi, wlbl);
        lv_obj_set_style_text_color(sb.wifi, wcol, 0);
    }
    // OBD label
    if (sb.obd) {
        bool conn = obd_is_connected();
        lv_label_set_text(sb.obd, conn ? LV_SYMBOL_BLUETOOTH " OBD" : LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(sb.obd,
            conn ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
    }
}

void timer_live_update(lv_timer_t * /*t*/) {
    // Update status bars
    update_sb(sb_menu);
    update_sb(sb_sub);

    // ---- Timing screen widgets (all null-checked) ----
    if (tw.speed_lbl) {
        char buf[16];
        float spd = g_state.units == 0
                    ? g_state.gps.speed_kmh
                    : g_state.gps.speed_kmh * 0.621371f;
        snprintf(buf, sizeof(buf), "%.0f", spd);
        lv_label_set_text(tw.speed_lbl, buf);
    }
    if (tw.laptime_lbl) {
        uint32_t ms = 0;
        if (g_state.timing.timing_active) {
            ms = millis() - g_state.timing.lap_start_ms;
        } else if (g_state.session.lap_count > 0) {
            ms = g_state.session.laps[g_state.session.lap_count - 1].total_ms;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%u:%05.2f",
                 ms / 60000, fmod(ms / 1000.0f, 60.0f));
        lv_label_set_text(tw.laptime_lbl, buf);
    }
    if (tw.bestlap_lbl && g_state.session.lap_count > 0) {
        uint32_t ms = g_state.session.laps[g_state.session.best_lap_idx].total_ms;
        if (ms > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u:%05.2f",
                     ms / 60000, fmod(ms / 1000.0f, 60.0f));
            lv_label_set_text(tw.bestlap_lbl, buf);
        }
    }
    if (tw.delta_lbl) {
        int32_t d = g_state.timing.live_delta_ms;
        char buf[16];
        snprintf(buf, sizeof(buf), "%+.2f s", d / 1000.0f);
        lv_label_set_text(tw.delta_lbl, buf);
        lv_obj_set_style_text_color(tw.delta_lbl,
            d < 0 ? lv_color_hex(0x00CC66)
                  : (d > 0 ? lv_color_hex(0xFF4444) : lv_color_hex(0xFFFFFF)), 0);
    }
    if (tw.delta_bar_fill && tw.delta_bar_lbl) {
        int32_t d     = g_state.timing.live_delta_ms;
        int32_t scale = timing_get_delta_scale();
        const int HALF = 396;
        int fill_w = (int)(fabsf((float)d) / (float)scale * HALF);
        if (fill_w > HALF) fill_w = HALF;
        if (d == 0 || !g_state.timing.timing_active) {
            lv_obj_set_size(tw.delta_bar_fill, 0, 22);
        } else if (d > 0) {
            lv_obj_set_style_bg_color(tw.delta_bar_fill, lv_color_hex(0xFF4444), 0);
            lv_obj_set_size(tw.delta_bar_fill, fill_w, 22);
            lv_obj_set_pos(tw.delta_bar_fill, 400 - fill_w, 0);
        } else {
            lv_obj_set_style_bg_color(tw.delta_bar_fill, lv_color_hex(0x00CC66), 0);
            lv_obj_set_size(tw.delta_bar_fill, fill_w, 22);
            lv_obj_set_pos(tw.delta_bar_fill, 400, 0);
        }
        // Label: delta + scale indicator (e.g. "+1.23 s [5s]")
        const char *scale_tag = scale == 2000  ? "[2s]"  :
                                scale == 3000  ? "[3s]"  :
                                scale == 5000  ? "[5s]"  :
                                scale == 10000 ? "[10s]" : "[20s]";
        char dbuf[24];
        if (!g_state.timing.timing_active || d == 0) {
            snprintf(dbuf, sizeof(dbuf), "\xC2\xB10.00 s  %s", scale_tag);
        } else {
            snprintf(dbuf, sizeof(dbuf), "%+.2f s  %s", d / 1000.0f, scale_tag);
        }
        lv_label_set_text(tw.delta_bar_lbl, dbuf);
        lv_obj_set_style_text_color(tw.delta_bar_lbl,
            d < 0 ? lv_color_hex(0x00CC66)
                  : (d > 0 ? lv_color_hex(0xFF4444) : lv_color_hex(0xFFFFFF)), 0);
    }
    if (tw.lap_nr_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)g_state.timing.lap_number);
        lv_label_set_text(tw.lap_nr_lbl, buf);
    }
    // Sectors — live running time for current sector, completed for past, last lap when idle
    {
        const LiveTiming &lt  = g_state.timing;
        const LapSession &sess = g_state.session;
        uint32_t now = millis();

        // Helper: format sector ms as "S<n> M.CS" (e.g. "S1 1.45")
        auto sec_fmt = [](char *b, size_t len, uint8_t n, uint32_t ms) {
            snprintf(b, len, "S%u %u.%02u", (unsigned)n,
                     ms / 1000, (ms % 1000) / 10);
        };

        if (lt.in_lap) {
            // in-progress lap: completed sectors stored at sess.laps[sess.lap_count]
            const uint32_t *s_ms = sess.laps[sess.lap_count].sector_ms;
            uint8_t cs      = lt.current_sector;
            uint32_t run_ms = now - lt.sector_start_ms;

            // Sector 1 (index 0)
            if (tw.sec1_lbl) {
                char buf[16];
                if (cs > 0) {
                    sec_fmt(buf, sizeof(buf), 1, s_ms[0]);
                    lv_obj_set_style_text_color(tw.sec1_lbl, BRL_CLR_TEXT_DIM, 0);
                } else {
                    snprintf(buf, sizeof(buf), "> %u.%02u", run_ms/1000, (run_ms%1000)/10);
                    lv_obj_set_style_text_color(tw.sec1_lbl, BRL_CLR_ACCENT, 0);
                }
                lv_label_set_text(tw.sec1_lbl, buf);
            }
            // Sector 2 (index 1)
            if (tw.sec2_lbl) {
                char buf[16];
                if (cs > 1) {
                    sec_fmt(buf, sizeof(buf), 2, s_ms[1]);
                    lv_obj_set_style_text_color(tw.sec2_lbl, BRL_CLR_TEXT_DIM, 0);
                } else if (cs == 1) {
                    snprintf(buf, sizeof(buf), "> %u.%02u", run_ms/1000, (run_ms%1000)/10);
                    lv_obj_set_style_text_color(tw.sec2_lbl, BRL_CLR_ACCENT, 0);
                } else {
                    strncpy(buf, "---", sizeof(buf));
                    lv_obj_set_style_text_color(tw.sec2_lbl, BRL_CLR_TEXT_DIM, 0);
                }
                lv_label_set_text(tw.sec2_lbl, buf);
            }
            // Sector 3 (index 2)
            if (tw.sec3_lbl) {
                char buf[16];
                if (cs > 2) {
                    sec_fmt(buf, sizeof(buf), 3, s_ms[2]);
                    lv_obj_set_style_text_color(tw.sec3_lbl, BRL_CLR_TEXT_DIM, 0);
                } else if (cs == 2) {
                    snprintf(buf, sizeof(buf), "> %u.%02u", run_ms/1000, (run_ms%1000)/10);
                    lv_obj_set_style_text_color(tw.sec3_lbl, BRL_CLR_ACCENT, 0);
                } else {
                    strncpy(buf, "---", sizeof(buf));
                    lv_obj_set_style_text_color(tw.sec3_lbl, BRL_CLR_TEXT_DIM, 0);
                }
                lv_label_set_text(tw.sec3_lbl, buf);
            }
        } else if (sess.lap_count > 0) {
            // Not in lap — show last completed lap's sector times
            const RecordedLap &last = sess.laps[sess.lap_count - 1];
            if (tw.sec1_lbl && last.sector_ms[0] > 0) {
                char buf[16]; sec_fmt(buf, sizeof(buf), 1, last.sector_ms[0]);
                lv_label_set_text(tw.sec1_lbl, buf);
                lv_obj_set_style_text_color(tw.sec1_lbl, BRL_CLR_TEXT, 0);
            }
            if (tw.sec2_lbl && last.sector_ms[1] > 0) {
                char buf[16]; sec_fmt(buf, sizeof(buf), 2, last.sector_ms[1]);
                lv_label_set_text(tw.sec2_lbl, buf);
                lv_obj_set_style_text_color(tw.sec2_lbl, BRL_CLR_TEXT, 0);
            }
            if (tw.sec3_lbl && last.sector_ms[2] > 0) {
                char buf[16]; sec_fmt(buf, sizeof(buf), 3, last.sector_ms[2]);
                lv_label_set_text(tw.sec3_lbl, buf);
                lv_obj_set_style_text_color(tw.sec3_lbl, BRL_CLR_TEXT, 0);
            }
        }
    }
    // OBD widgets
    if (tw.rpm_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f rpm", g_state.obd.rpm);
        lv_label_set_text(tw.rpm_lbl, buf);
    }
    if (tw.throttle_lbl) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%.0f %%", g_state.obd.throttle_pct);
        lv_label_set_text(tw.throttle_lbl, buf);
    }
    if (tw.boost_lbl) {
        // boost_kpa is MAP absolute; show as boost (subtract ~101 kPa)
        float boost = g_state.obd.boost_kpa - 101.3f;
        char buf[16];
        snprintf(buf, sizeof(buf), "%+.0f kPa", boost);
        lv_label_set_text(tw.boost_lbl, buf);
    }
    if (tw.lambda_lbl) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%.2f λ", g_state.obd.lambda);
        lv_label_set_text(tw.lambda_lbl, buf);
    }
    if (tw.brake_lbl) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%.0f %%", g_state.obd.brake_pct);
        lv_label_set_text(tw.brake_lbl, buf);
    }
    if (tw.coolant_lbl) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%.0f °C", g_state.obd.coolant_temp_c);
        lv_label_set_text(tw.coolant_lbl, buf);
    }
    // gear_lbl and steering_lbl: ObdData has no gear field; use steering_angle
    if (tw.gear_lbl) {
        // Gear not in ObdData — show placeholder
        lv_label_set_text(tw.gear_lbl, "-");
    }
    if (tw.steering_lbl) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%+.0f°", g_state.obd.steering_angle);
        lv_label_set_text(tw.steering_lbl, buf);
    }
    // Track name
    if (tw.track_name_lbl) {
        if (g_state.active_track_idx >= 0 && g_state.active_track_idx < track_total_count()) {
            lv_label_set_text(tw.track_name_lbl,
                              track_get(g_state.active_track_idx)->name);
        }
    }
    // Start/stop button label
    if (tw.start_btn_lbl) {
        lv_label_set_text(tw.start_btn_lbl,
                          g_state.timing.timing_active
                          ? LV_SYMBOL_STOP " STOP"
                          : LV_SYMBOL_PLAY " START");
    }

    // ---- Settings screen labels (null-checked) ----
    if (set_obd_status_lbl) {
        bool conn = obd_is_connected();
        lv_label_set_text(set_obd_status_lbl,
                          conn ? tr(TR_CONNECTED)
                               : (obd_is_scanning() ? tr(TR_SCANNING) : tr(TR_NOT_CONNECTED)));
        lv_obj_set_style_text_color(set_obd_status_lbl,
            conn ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
    }
    if (set_obd_btn) {
        bool conn = obd_is_connected();
        char obd_btn_buf[48];
        snprintf(obd_btn_buf, sizeof(obd_btn_buf),
                 conn ? LV_SYMBOL_CLOSE " %s" : LV_SYMBOL_BLUETOOTH " %s",
                 conn ? tr(TR_DISCONNECT_BTN) : tr(TR_CONNECT_BTN));
        lv_label_set_text(lv_obj_get_child(set_obd_btn, 0), obd_btn_buf);
    }
    if (set_wifi_ap_status_lbl) {
        bool ap_on = (g_state.wifi_mode == BRL_WIFI_AP || g_state.wifi_mode == BRL_WIFI_OTA);
        lv_label_set_text(set_wifi_ap_status_lbl, ap_on ? tr(TR_WIFI_AP_ON) : tr(TR_WIFI_AP_OFF));
        lv_obj_set_style_text_color(set_wifi_ap_status_lbl,
            ap_on ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
        if (set_wifi_ap_sw) {
            if (ap_on && !lv_obj_has_state(set_wifi_ap_sw, LV_STATE_CHECKED))
                lv_obj_add_state(set_wifi_ap_sw, LV_STATE_CHECKED);
            else if (!ap_on && lv_obj_has_state(set_wifi_ap_sw, LV_STATE_CHECKED))
                lv_obj_remove_state(set_wifi_ap_sw, LV_STATE_CHECKED);
        }
    }
    if (set_wifi_sta_status_lbl) {
        bool sta_on = (g_state.wifi_mode == BRL_WIFI_STA);
        lv_label_set_text(set_wifi_sta_status_lbl,
                          sta_on ? tr(TR_CONNECTED) : tr(TR_NOT_CONNECTED));
        lv_obj_set_style_text_color(set_wifi_sta_status_lbl,
            sta_on ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
    }
}


// ---------------------------------------------------------------------------
// Section 9: app_init() and app_tick()
// ---------------------------------------------------------------------------
void app_init() {
    g_state = {};
    g_state.active_track_idx = -1;

    dash_config_load();
    g_state.language = g_dash_cfg.language;
    g_state.units    = g_dash_cfg.units;
    i18n_set_language(g_state.language);

    // Create the persistent menu screen now (so timing_screen_build can
    // reference menu_screen_show even before the menu is visible).
    build_menu_screen();

    // Show splash, then load menu screen when done
    splash_show(3000, []() {
        lv_screen_load(s_scr_menu);
    });
}

void app_tick() {
    // Reserved for future per-loop work (currently handled by lv_timer_handler)
}
