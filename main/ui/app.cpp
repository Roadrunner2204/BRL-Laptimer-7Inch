/**
 * BRL Laptimer — Application UI
 *
 * Screen flow:
 *   Splash → Main Menu → [TIMING]     → Track Selection → Timing Screen
 *                      → [STRECKEN]   → Track Selection (+ Neue Strecke Wizard)
 *                      → [VERLAUF]    → History
 *                      → [EINSTELLUNGEN] → Settings
 */

#include "compat.h"
static const char *TAG = "app";
#include <math.h>
#include <lvgl.h>
#include "brl_fonts.h"
#include "i18n.h"
#include "app.h"
#include "theme.h"
#include "screen_splash.h"
#include "screen_timing.h"
#include "dash_config.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include "../timing/lap_timer.h"
#include "../obd/obd_bt.h"
#include "../can/can_bus.h"
#include "../gps/gps.h"
#include "driver/gpio.h"
#include "../wifi/wifi_mgr.h"
#include "../camera_link/cam_link.h"
#include "../storage/session_store.h"
#include "../sensors/analog_in.h"
#include "../storage/sd_mgr.h"
#include "../storage/track_update.h"
#include "../data/car_profile.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Global application state
// ---------------------------------------------------------------------------
AppState g_state = {};

// True while the current session has NOT yet produced a lap faster than the
// stored all-time best for the active track. Drives purple coloring on the
// sector slots and delta bar. Updated by lap_timer (track-select + finish).
bool g_chasing_record = false;

// ---------------------------------------------------------------------------
// Status bar handles (one set per long-lived screen)
// ---------------------------------------------------------------------------
struct SbH { lv_obj_t *gps, *wifi, *obd, *rec; };
static SbH sb_menu   = {};   // menu screen — cached, never rebuilt
static SbH sb_sub    = {};   // tracks / history / settings — cleared on back
static SbH sb_timing = {};   // timing screen — repopulated after every build

// ---------------------------------------------------------------------------
// Screen pointers
// ---------------------------------------------------------------------------
static lv_obj_t *s_scr_menu  = nullptr;   // cached forever
static lv_obj_t *s_scr_sub   = nullptr;   // rebuilt each time

// ── QWERTZ keyboard maps (must have static lifetime) ────────────────────────
// LV_SYMBOL_SHIFT is private in lv_keyboard.c — use the raw UTF-8 value (U+F12A)
#define _SHF "\xef\x84\xaa"
#define _KBF(w) static_cast<lv_btnmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS|(w))

static const char * const s_qwertz_lc[] = {
    "1","2","3","4","5","6","7","8","9","0",LV_SYMBOL_BACKSPACE,"\n",
    "q","w","e","r","t","z","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l",LV_SYMBOL_NEW_LINE,"\n",
    _SHF,"y","x","c","v","b","n","m",".",",",_SHF,"\n",
    "1#"," ",LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,""
};
static const char * const s_qwertz_uc[] = {
    "1","2","3","4","5","6","7","8","9","0",LV_SYMBOL_BACKSPACE,"\n",
    "Q","W","E","R","T","Z","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L",LV_SYMBOL_NEW_LINE,"\n",
    _SHF,"Y","X","C","V","B","N","M",".",",",_SHF,"\n",
    "abc"," ",LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,""
};
static const lv_btnmatrix_ctrl_t s_qwertz_ctrl[] = {
    _KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(7),  // row1
    _KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),          // row2
    _KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(7),          // row3
    _KBF(7),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(5),_KBF(7),  // row4
    _KBF(5),_KBF(7),_KBF(5),_KBF(5),                                                             // row5
};
#undef _SHF
#undef _KBF

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

// WiFi AP config dialog (modal)
static lv_obj_t *s_ap_dialog  = nullptr;
static lv_obj_t *s_ap_ta_ssid = nullptr;
static lv_obj_t *s_ap_ta_pass = nullptr;
static lv_obj_t *s_ap_kb      = nullptr;

// Track creator state
static lv_obj_t *s_tc_name     = nullptr;
static lv_obj_t *s_tc_sf1_lat  = nullptr, *s_tc_sf1_lon = nullptr;
static lv_obj_t *s_tc_sf2_lat  = nullptr, *s_tc_sf2_lon = nullptr;
static lv_obj_t *s_tc_fin_box  = nullptr;  // shown for A-B
static lv_obj_t *s_tc_fin1_lat = nullptr, *s_tc_fin1_lon = nullptr;
static lv_obj_t *s_tc_fin2_lat = nullptr, *s_tc_fin2_lon = nullptr;
static lv_obj_t *s_tc_kb       = nullptr;   // full text keyboard (track name)
static lv_obj_t *s_tc_kb_num   = nullptr;   // numeric keyboard (coordinates)
static lv_obj_t *s_tc_type_circ_btn = nullptr;
static lv_obj_t *s_tc_type_ab_btn   = nullptr;
static bool      s_tc_is_circuit    = true;
static lv_obj_t *s_tc_sec_lat[MAX_SECTORS] = {};
static lv_obj_t *s_tc_sec_lon[MAX_SECTORS] = {};
static int       s_tc_sec_count = 0;
static lv_obj_t *s_tc_sec_container = nullptr;
static lv_obj_t *s_tc_add_sec_btn   = nullptr;
static int       s_tc_edit_idx      = -1;   // -1 = new, >=0 = editing existing track

// Track list filters
static char        s_filter_country[32] = {};  // empty = show all countries
static char        s_filter_name[48]    = {};  // empty = show all names
static lv_obj_t   *s_tracks_search_kb   = nullptr;  // reset on each rebuild

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void open_tracks_screen();
static void open_track_creator(lv_obj_t *parent_scroll, int edit_idx = -1);
static void open_history_screen();
static void open_session_detail_screen(const char *session_id,
                                       const char *session_name);
static void open_settings_screen();
static void open_analog_screen();
static void open_can_channels_screen();
static void open_can_channel_edit(int slot_idx);  // -1 = new sensor

// ============================================================================
// SECTION 1 — SHARED HELPERS
// ============================================================================

// Build a 40px status bar on any screen; fills *out with label handles
static void build_sb(lv_obj_t *scr, SbH *out) {
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, BRL_SCREEN_W, 40);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, BRL_CLR_STATUSBAR, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(bar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(bar, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    out->gps = lv_label_create(bar);
    lv_label_set_text(out->gps, LV_SYMBOL_GPS);
    brl_style_label(out->gps, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(out->gps, 8, 12);

    out->wifi = lv_label_create(bar);
    lv_label_set_text(out->wifi, LV_SYMBOL_WIFI " --");
    brl_style_label(out->wifi, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(out->wifi, 180, 12);

    out->obd = lv_label_create(bar);
    lv_label_set_text(out->obd, LV_SYMBOL_DRIVE);
    brl_style_label(out->obd, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(out->obd, 350, 12);

    out->rec = lv_label_create(bar);
    lv_label_set_text(out->rec, "");
    brl_style_label(out->rec, &BRL_FONT_14, BRL_CLR_DANGER);
    lv_obj_set_pos(out->rec, 520, 12);
}

// Build a sub-screen header bar (back + title) at y=40, h=50
static lv_obj_t *build_sub_header(lv_obj_t *scr, const char *title,
                                   lv_event_cb_t back_cb,
                                   lv_obj_t **action_btn_out = nullptr,
                                   const char *action_label = nullptr) {
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, BRL_SCREEN_W, 50);
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

// Content area: y=90, h=510 (below status bar + header)
static lv_obj_t *build_content_area(lv_obj_t *scr, bool scrollable = true) {
    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_set_size(area, BRL_SCREEN_W, BRL_SCREEN_H - 90);
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
    sb_timing = {};  // timing screen labels are gone once menu is shown
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
    s_ap_dialog = s_ap_ta_ssid = s_ap_ta_pass = s_ap_kb = nullptr;
    s_tc_kb = nullptr;
    s_tc_kb_num = nullptr;

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
        s_ap_dialog = s_ap_ta_ssid = s_ap_ta_pass = s_ap_kb = nullptr;
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
        sb_timing = {tw.sb_gps_lbl, nullptr, tw.sb_obd_lbl, nullptr};
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
    lv_obj_set_size(strip, BRL_SCREEN_W, 48);
    lv_obj_set_pos(strip, 0, 40);
    lv_obj_set_style_bg_color(strip, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(strip, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(strip, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(strip, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(strip);
    lv_label_set_text(brand, "BAVARIAN RACELABS  —  BRL LAPTIMER");
    brl_style_label(brand, &BRL_FONT_20, BRL_CLR_ACCENT);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, 0);

    // Main menu tiles — 2×2 grid centered on the screen.
    const int TW = 500, TH = 248, GAP = 8;
    const int X0 = (BRL_SCREEN_W - (2*TW + GAP)) / 2;
    const int Y0 = 96;
    struct { const char *icon; TrKey label_key; TrKey sub_key; lv_event_cb_t cb; } tiles[4] = {
        { LV_SYMBOL_PLAY,     TR_TILE_TIMING,   TR_TILE_TIMING_SUB,   cb_tile_timing   },
        { LV_SYMBOL_GPS,      TR_TILE_TRACKS,   TR_TILE_TRACKS_SUB,   cb_tile_tracks   },
        { LV_SYMBOL_DRIVE,    TR_TILE_HISTORY,  TR_TILE_HISTORY_SUB,  cb_tile_history  },
        { LV_SYMBOL_SETTINGS, TR_TILE_SETTINGS, TR_TILE_SETTINGS_SUB, cb_tile_settings },
    };

    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
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
        lv_obj_set_style_text_font(ico, &BRL_FONT_64, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ico, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
        lv_obj_align(ico, LV_ALIGN_CENTER, 0, -40);

        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, tr(tiles[i].label_key));
        brl_style_label(lbl, &BRL_FONT_24, BRL_CLR_TEXT);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 40);

        lv_obj_t *sub = lv_label_create(tile);
        lv_label_set_text(sub, tr(tiles[i].sub_key));
        brl_style_label(sub, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 72);
    }

    if (!is_rebuild) {
        lv_screen_load(s_scr_menu);
    }
    if (!s_timer_created) {
        lv_timer_create([](lv_timer_t *t){
            extern void timer_live_update(lv_timer_t*);
            timer_live_update(t);
        }, 100, nullptr);

        // Fast (33 ms ≈ 30 Hz) timer — updates ONLY the laptime label(s)
        // on the timing screen so the hundredths digit ticks visibly.
        // Guarded with strcmp so we don't re-flag the label dirty when
        // the formatted text is identical (96-pt font redraw is expensive).
        lv_timer_create([](lv_timer_t * /*t*/) {
            lv_obj_t *active_scr = lv_screen_active();
            if (!tw.z1_val[0]) return;
            if (!active_scr || lv_obj_get_screen(tw.z1_val[0]) != active_scr) return;
            const LiveTiming &lt   = g_state.timing;
            const LapSession &sess = g_state.session;
            uint32_t ms = 0;
            if (lt.in_lap) ms = millis() - lt.lap_start_ms;
            else if (sess.lap_count > 0)
                ms = sess.laps[sess.lap_count - 1].total_ms;
            else return;
            char buf[16];
            snprintf(buf, sizeof(buf), "%u:%05.2f",
                     (unsigned)(ms / 60000), fmod(ms / 1000.0f, 60.0f));
            for (int i = 0; i < Z1_SLOTS; i++) {
                if (g_dash_cfg.z1[i] == FIELD_LAPTIME && tw.z1_val[i]) {
                    const char *cur = lv_label_get_text(tw.z1_val[i]);
                    if (cur && strcmp(cur, buf) == 0) continue;
                    lv_label_set_text(tw.z1_val[i], buf);
                }
            }
        }, 33, nullptr);

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
    int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
    g_state.active_track_idx = idx;
    lap_timer_set_track(idx);
    const TrackDef *td = track_get(idx);
    ESP_LOGI(TAG, "[APP] Track selected: %s\n", td ? td->name : "?");
    timing_screen_open();
    sb_timing = {tw.sb_gps_lbl, nullptr, tw.sb_obd_lbl, nullptr};
    // timing_screen_open loads a new LVGL screen — sub screen will be cleaned up on next menu_show
    s_scr_sub = nullptr;
}

static void cb_open_creator(lv_event_t *e) {
    lv_obj_t *scroll = (lv_obj_t*)lv_event_get_user_data(e);
    if (scroll) open_track_creator(scroll, -1);
}

static void cb_edit_track(lv_event_t *e) {
    lv_obj_t *scroll = (lv_obj_t*)lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
    if (scroll) open_track_creator(scroll, idx);
}

static void cb_delete_track(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
    if (idx < TRACK_DB_BUILTIN_COUNT) return;   // no built-in deletion
    int u_slot = idx - TRACK_DB_BUILTIN_COUNT;
    // Bundle tracks (loaded from Tracks.tbrl) are read-only — reject.
    if (u_slot >= g_user_track_count) return;
    // Adjust active track pointer
    if (g_state.active_track_idx == idx)
        g_state.active_track_idx = -1;
    else if (g_state.active_track_idx > idx)
        g_state.active_track_idx--;
    session_store_delete_user_track(u_slot);
    open_tracks_screen();
}

static void open_tracks_screen() {
    // Old search keyboard was a child of the previous screen and will be
    // deleted along with it — our stored pointer is about to dangle.
    s_tracks_search_kb = nullptr;

    int n = track_total_count();

    // ── Collect distances (heap-allocated so we can handle 900+ bundle) ───
    int    *sorted = (int *)   heap_caps_malloc(sizeof(int)    * n, MALLOC_CAP_SPIRAM);
    double *dist   = (double *)heap_caps_malloc(sizeof(double) * n, MALLOC_CAP_SPIRAM);
    if (!sorted || !dist) {
        if (sorted) free(sorted);
        if (dist)   free(dist);
        return;
    }
    for (int i = 0; i < n; i++) {
        sorted[i] = i;
        const TrackDef *td = track_get(i);
        if (td && g_state.gps.valid) {
            double mlat = (td->sf_lat1 + td->sf_lat2) * 0.5;
            double mlon = (td->sf_lon1 + td->sf_lon2) * 0.5;
            dist[i] = haversine_km(g_state.gps.lat, g_state.gps.lon, mlat, mlon);
        } else dist[i] = 1e9;
    }
    // Sort by distance (insertion sort OK — O(n²) acceptable for n ≤ ~2000)
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && dist[sorted[j]] < dist[sorted[j-1]]; j--)
            { int t=sorted[j]; sorted[j]=sorted[j-1]; sorted[j-1]=t; }

    // ── Collect unique countries from ALL tracks (not just first 60) ──────
    #define MAX_UNIQUE_COUNTRIES 96
    static char countries[MAX_UNIQUE_COUNTRIES][32] = {};
    int  n_countries = 0;
    for (int i = 0; i < n; i++) {
        const TrackDef *td = track_get(i);
        if (!td || !td->country[0]) continue;
        bool found = false;
        for (int c = 0; c < n_countries; c++)
            if (strcmp(countries[c], td->country) == 0) { found = true; break; }
        if (!found && n_countries < MAX_UNIQUE_COUNTRIES) {
            snprintf(countries[n_countries], 32, "%s", td->country);
            n_countries++;
        }
    }
    // Sort country list alphabetically (UX: easier to find a country)
    for (int i = 1; i < n_countries; i++)
        for (int j = i; j > 0 && strcasecmp(countries[j], countries[j-1]) < 0; j--) {
            char tmp[32]; memcpy(tmp, countries[j], 32);
            memcpy(countries[j], countries[j-1], 32);
            memcpy(countries[j-1], tmp, 32);
        }
    // Pin the "Custom" / "Benutzerdefiniert" label to the top of the dropdown
    // so user-created tracks are always one tap away, regardless of language
    // or how many imported countries there are.
    const char *custom = tr(TR_CUSTOM_COUNTRY);
    for (int i = 0; i < n_countries; i++) {
        if (strcmp(countries[i], custom) == 0 && i != 0) {
            char tmp[32]; memcpy(tmp, countries[i], 32);
            // Shift [0..i-1] down by one, then put custom at [0]
            memmove(countries[1], countries[0], 32 * (size_t)i);
            memcpy(countries[0], tmp, 32);
            break;
        }
    }

    // ── Build screen ──────────────────────────────────────────────────────
    lv_obj_t *new_btn = nullptr;
    char new_btn_label[48];
    snprintf(new_btn_label, sizeof(new_btn_label), LV_SYMBOL_PLUS " %s", tr(TR_NEW_TRACK));
    lv_obj_t *scr = make_sub_screen(tr(TR_SELECT_TRACK), cb_back_to_menu,
                                     &new_btn, new_btn_label);

    // Second header button: "UPDATE" — downloads the latest .tbrl bundle
    // from the BRL server (language picked automatically) and reloads it.
    // Placed to the left of "NEW TRACK".
    {
        lv_obj_t *hdr = new_btn ? lv_obj_get_parent(new_btn) : nullptr;
        if (hdr) {
            lv_obj_t *upd = lv_button_create(hdr);
            lv_obj_set_size(upd, 140, 38);
            lv_obj_align(upd, LV_ALIGN_RIGHT_MID, -6 - 140 - 8, 0);
            brl_style_btn(upd, BRL_CLR_SURFACE2);
            lv_obj_t *ul = lv_label_create(upd);
            lv_label_set_text(ul, tr(TR_TRACK_UPDATE));
            brl_style_label(ul, &BRL_FONT_14, BRL_CLR_TEXT);
            lv_obj_center(ul);
            lv_obj_set_user_data(upd, ul);   // label ref for busy-state swap
            lv_obj_add_event_cb(upd, [](lv_event_t *e) {
                lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
                lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(btn);

                if (g_state.wifi_mode != BRL_WIFI_STA) {
                    lv_obj_t *mb = lv_msgbox_create(nullptr);
                    lv_msgbox_add_title(mb, "WiFi");
                    lv_msgbox_add_text(mb,
                        "Fuer das Update zuerst mit Internet verbinden\n"
                        "(Einstellungen → WLAN → STA).");
                    lv_msgbox_add_close_button(mb);
                    return;
                }
                // Busy feedback while the HTTPS download runs
                if (lbl) lv_label_set_text(lbl, "...");
                lv_obj_add_state(btn, LV_STATE_DISABLED);

                xTaskCreate([](void * /*arg*/) {
                    extern int track_update_run_blocking(void);
                    int n = track_update_run_blocking();
                    lv_async_call([](void *argn) {
                        int r = (int)(intptr_t)argn;
                        lv_obj_t *mb = lv_msgbox_create(nullptr);
                        if (r > 0) {
                            char buf[96];
                            snprintf(buf, sizeof(buf),
                                     tr(TR_TRACK_UPDATE_OK), r);
                            lv_msgbox_add_title(mb, tr(TR_TRACK_UPDATE));
                            lv_msgbox_add_text(mb, buf);
                            lv_msgbox_add_close_button(mb);
                            open_tracks_screen();   // rebuilds with new list
                        } else {
                            lv_msgbox_add_title(mb, tr(TR_TRACK_UPDATE_FAIL));
                            lv_msgbox_add_text(mb,
                                "Download oder HDD-Schreiben fehlgeschlagen.");
                            lv_msgbox_add_close_button(mb);
                            open_tracks_screen();   // re-enables the button
                        }
                    }, (void*)(intptr_t)n);
                    vTaskDelete(nullptr);
                }, "trk_upd", 8192, nullptr, 4, nullptr);
            }, LV_EVENT_CLICKED, nullptr);
        }
    }

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

        // Filter row: [Country dropdown] [Search textarea] [Clear]
        lv_obj_t *filter_row = lv_obj_create(content);
        lv_obj_set_width(filter_row, LV_PCT(100));
        lv_obj_set_height(filter_row, 50);
        lv_obj_set_style_bg_opa(filter_row, LV_OPA_TRANSP, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(filter_row, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(filter_row, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_column(filter_row, 8, LV_STATE_DEFAULT);
        lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(filter_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(filter_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dd = lv_dropdown_create(filter_row);
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

        // ── Search textarea (by track name) ────────────────────────────
        lv_obj_t *search = lv_textarea_create(filter_row);
        lv_textarea_set_one_line(search, true);
        lv_textarea_set_placeholder_text(search, "Name suchen…");
        lv_textarea_set_text(search, s_filter_name);
        lv_obj_set_width(search, 360);
        lv_obj_set_height(search, 40);
        lv_obj_set_style_bg_color(search, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(search, BRL_CLR_TEXT, LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(search, &BRL_FONT_16, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(search, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(search, 0, LV_STATE_DEFAULT);

        // Clear button: one-tap reset of the name filter
        lv_obj_t *clr = lv_button_create(filter_row);
        lv_obj_set_size(clr, 44, 40);
        brl_style_btn(clr, BRL_CLR_SURFACE2);
        lv_obj_t *clr_lbl = lv_label_create(clr);
        lv_label_set_text(clr_lbl, LV_SYMBOL_CLOSE);
        brl_style_label(clr_lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_center(clr_lbl);
        lv_obj_add_event_cb(clr, [](lv_event_t * /*e*/) {
            s_filter_name[0] = '\0';
            open_tracks_screen();
        }, LV_EVENT_CLICKED, nullptr);

        // Pop up a keyboard when the search gains focus; hide on Enter.
        // Keyboard is a child of the active screen so it gets cleaned up
        // when we rebuild; the outer static pointer is cleared at the top
        // of open_tracks_screen() to avoid dangling references.
        lv_obj_add_event_cb(search, [](lv_event_t *e) {
            lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
            if (!s_tracks_search_kb) {
                s_tracks_search_kb = lv_keyboard_create(lv_screen_active());
                lv_obj_set_size(s_tracks_search_kb, BRL_SCREEN_W, 260);
                lv_obj_align(s_tracks_search_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
            }
            lv_keyboard_set_textarea(s_tracks_search_kb, ta);
            lv_obj_remove_flag(s_tracks_search_kb, LV_OBJ_FLAG_HIDDEN);
        }, LV_EVENT_FOCUSED, nullptr);

        lv_obj_add_event_cb(search, [](lv_event_t *e) {
            lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
            const char *txt = lv_textarea_get_text(ta);
            strncpy(s_filter_name, txt ? txt : "", sizeof(s_filter_name) - 1);
            s_filter_name[sizeof(s_filter_name) - 1] = '\0';
        }, LV_EVENT_VALUE_CHANGED, nullptr);

        // Enter on the on-screen keyboard → apply filter + rebuild screen.
        // open_tracks_screen() clears s_tracks_search_kb at its top, and
        // the old keyboard gets deleted along with the old sub-screen.
        lv_obj_add_event_cb(search, [](lv_event_t * /*e*/) {
            open_tracks_screen();
        }, LV_EVENT_READY, nullptr);
    }

    // ── GPS hint ──────────────────────────────────────────────────────────
    if (!g_state.gps.valid) {
        lv_obj_t *hint = lv_label_create(content);
        lv_label_set_text_fmt(hint, LV_SYMBOL_GPS "  %s", tr(TR_NO_GPS_HINT));
        brl_style_label(hint, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    }

    // ── Track rows ────────────────────────────────────────────────────────
    // With 900+ tracks in the bundle, rendering them all would blow up LVGL
    // (thousands of child widgets). Cap visible rows: if a country filter
    // is active, show all of that country (usually <50); otherwise show
    // the 80 nearest so the user can pick from the neighbourhood.
    int rendered = 0;
    const int RENDER_CAP = (s_filter_country[0] != '\0' || s_filter_name[0] != '\0')
                           ? 2048 : 80;
    for (int s = 0; s < n && rendered < RENDER_CAP; s++) {
        int idx = sorted[s];
        const TrackDef *td = track_get(idx);
        if (!td) continue;

        // Skip bundle entries that are shadowed by a user-created edit
        // with the same name — user version is shown instead, no dupes.
        if (track_is_shadowed(idx)) continue;

        // Apply country filter
        if (s_filter_country[0] != '\0' && strcmp(td->country, s_filter_country) != 0)
            continue;

        // Apply case-insensitive name substring filter
        if (s_filter_name[0] != '\0') {
            // Simple strcasestr (ESP-IDF's libc doesn't expose it on riscv)
            const char *hay = td->name;
            const char *needle = s_filter_name;
            size_t nlen = strlen(needle);
            bool hit = false;
            for (; *hay; hay++) {
                if (strncasecmp(hay, needle, nlen) == 0) { hit = true; break; }
            }
            if (!hit) continue;
        }
        rendered++;

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
        int sel_w = (BRL_SCREEN_W - 16) - right_buttons_w;
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

    free(sorted);
    free(dist);
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

    // Focus → show numeric keyboard, hide text keyboard
    auto kb_focus = [](lv_event_t *ev){
        if (s_tc_kb)     lv_obj_add_flag(s_tc_kb, LV_OBJ_FLAG_HIDDEN);
        if (s_tc_kb_num) {
            lv_keyboard_set_textarea(s_tc_kb_num, (lv_obj_t*)lv_event_get_target(ev));
            lv_obj_remove_flag(s_tc_kb_num, LV_OBJ_FLAG_HIDDEN);
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
    char sec_lbl[16]; snprintf(sec_lbl, sizeof(sec_lbl), "S%d", s_tc_sec_count+1);
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

    // ── Case B: editing a bundle track → in-place edit (session-only) ────
    } else if (s_tc_edit_idx >= TRACK_DB_BUILTIN_COUNT + g_user_track_count
               && g_bundle_tracks) {
        int b = s_tc_edit_idx - TRACK_DB_BUILTIN_COUNT - g_user_track_count;
        if (b >= 0 && b < g_bundle_track_count) {
            TrackDef &td = g_bundle_tracks[b];
            // Keep name/country/length, only overwrite user-editable coords.
            td.is_circuit = s_tc_is_circuit;
            tc_fill_coords(td);
            // Bundle is PSRAM-only — edits live until next boot (bundle is
            // reparsed from /sdcard/Tracks.tbrl fresh). We deliberately do
            // NOT persist, because that would drift from the canonical
            // server-maintained catalog. Users who want persistence should
            // instead add a new user track.
            save_idx = s_tc_edit_idx;
        } else {
            return;
        }

    // ── Case C: new track or editing an existing user track ──────────────
    } else {
        int u_slot = -1;
        if (s_tc_edit_idx >= TRACK_DB_BUILTIN_COUNT &&
            s_tc_edit_idx < TRACK_DB_BUILTIN_COUNT + g_user_track_count)
            u_slot = s_tc_edit_idx - TRACK_DB_BUILTIN_COUNT;
        if (u_slot < 0) {
            if (g_user_track_count >= MAX_USER_TRACKS) return;
            u_slot = g_user_track_count;
        }
        TrackDef &td = g_user_tracks[u_slot];
        // Preserve existing name/country when editing; zero only on create
        bool is_new = (u_slot == g_user_track_count);
        if (is_new) {
            memset(&td, 0, sizeof(td));
            strncpy(td.name, name, sizeof(td.name)-1);
            strncpy(td.country, tr(TR_CUSTOM_COUNTRY), sizeof(td.country)-1);
        } else {
            // Allow name edit too
            strncpy(td.name, name, sizeof(td.name)-1);
        }
        td.is_circuit   = s_tc_is_circuit;
        td.user_created = true;
        tc_fill_coords(td);
        if (is_new) g_user_track_count++;
        session_store_save_user_track(&td);
        save_idx = TRACK_DB_BUILTIN_COUNT + u_slot;
    }

    g_state.active_track_idx = save_idx;
    lap_timer_set_track(save_idx);
    s_tc_edit_idx = -1;

    // Null keyboard handles — parent (s_scr_sub) will be deleted in deferred timer
    s_tc_kb     = nullptr;
    s_tc_kb_num = nullptr;

    // Defer screen transition: returning from the event callback first prevents
    // stack overflow (timing_screen_build is heavy) and watchdog issues from the
    // SD write above. The one-shot timer fires in the next LVGL cycle.
    lv_timer_t *tmr = lv_timer_create([](lv_timer_t *t) {
        lv_timer_delete(t);
        sb_sub = {};   // clear BEFORE delete — labels are children of s_scr_sub
        set_obd_status_lbl = set_obd_btn = nullptr;
        set_wifi_ap_sw = set_wifi_ap_status_lbl = set_wifi_sta_status_lbl = nullptr;
        s_ap_dialog = s_ap_ta_ssid = s_ap_ta_pass = s_ap_kb = nullptr;
        if (s_scr_sub) {
            lv_obj_delete(s_scr_sub);
            s_scr_sub = nullptr;
        }
        timing_screen_rebuild();
        sb_timing = {tw.sb_gps_lbl, nullptr, tw.sb_obd_lbl, nullptr};
    }, 50, nullptr);
    lv_timer_set_repeat_count(tmr, 1);
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
    // Diagnostic: which physical entry is being edited and which coords
    // we're about to populate the textareas with. If you tap "Nurburgring
    // GP" expecting your edit but see bundle coords here, the dedupe
    // missed because of a hidden name difference (trailing space etc.).
    if (edit_td) {
        int u_max = TRACK_DB_BUILTIN_COUNT + g_user_track_count;
        const char *kind = (edit_idx < TRACK_DB_BUILTIN_COUNT) ? "BUILTIN"
                          : (edit_idx < u_max) ? "USER" : "BUNDLE";
        ESP_LOGI("open_tc",
            "edit_idx=%d (%s) name='%s' sf=[%.6f, %.6f -> %.6f, %.6f]",
            edit_idx, kind, edit_td->name,
            edit_td->sf_lat1, edit_td->sf_lon1,
            edit_td->sf_lat2, edit_td->sf_lon2);
    }

    // Clear the existing list and build creator form in place
    lv_obj_clean(scroll);

    auto section = [&](const char *t){ mk_section_label(scroll, t); };
    auto kb_focus_name = [](lv_event_t *ev){
        if (s_tc_kb_num) lv_obj_add_flag(s_tc_kb_num, LV_OBJ_FLAG_HIDDEN);
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

    // Full text keyboard — bottom, for track name field
    s_tc_kb = lv_keyboard_create(lv_screen_active());
    lv_keyboard_set_map(s_tc_kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_qwertz_lc, s_qwertz_ctrl);
    lv_keyboard_set_map(s_tc_kb, LV_KEYBOARD_MODE_TEXT_UPPER, s_qwertz_uc, s_qwertz_ctrl);
    lv_obj_set_size(s_tc_kb, BRL_SCREEN_W, 266);
    lv_obj_align(s_tc_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_tc_kb, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_tc_kb, [](lv_event_t *ev){
        lv_obj_add_flag((lv_obj_t*)lv_event_get_target(ev), LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(s_tc_kb, LV_OBJ_FLAG_HIDDEN);

    // Numeric keyboard — bottom-right, for coordinate fields
    s_tc_kb_num = lv_keyboard_create(lv_screen_active());
    lv_keyboard_set_mode(s_tc_kb_num, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(s_tc_kb_num, 340, 400);
    lv_obj_align(s_tc_kb_num, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_tc_kb_num, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_tc_kb_num, [](lv_event_t *ev){
        lv_obj_add_flag((lv_obj_t*)lv_event_get_target(ev), LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(s_tc_kb_num, LV_OBJ_FLAG_HIDDEN);

    // Pre-fill fields when editing an existing track
    if (edit_td) {
        lv_textarea_set_text(s_tc_name, edit_td->name);

        // Circuit/A-B button state
        if (!edit_td->is_circuit) {
            lv_obj_set_style_bg_color(s_tc_type_circ_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(s_tc_type_ab_btn,   BRL_CLR_ACCENT,   LV_STATE_DEFAULT);
            if (s_tc_fin_box) lv_obj_remove_flag(s_tc_fin_box, LV_OBJ_FLAG_HIDDEN);
        }

        // S/F coordinates — set + force-invalidate so LVGL definitely
        // re-rasterises the textarea label even if its internal dirty
        // tracking missed the update.
        char buf[24];
        auto fill = [&](lv_obj_t *ta, double v) {
            char b[24];
            snprintf(b, sizeof(b), "%.7f", v);
            lv_textarea_set_text(ta, b);
            lv_obj_invalidate(ta);
        };
        fill(s_tc_sf1_lat, edit_td->sf_lat1);
        fill(s_tc_sf1_lon, edit_td->sf_lon1);
        fill(s_tc_sf2_lat, edit_td->sf_lat2);
        fill(s_tc_sf2_lon, edit_td->sf_lon2);
        (void)buf;

        ESP_LOGI("open_tc", "Textareas after fill: sf1=[%s, %s] sf2=[%s, %s]",
                 lv_textarea_get_text(s_tc_sf1_lat),
                 lv_textarea_get_text(s_tc_sf1_lon),
                 lv_textarea_get_text(s_tc_sf2_lat),
                 lv_textarea_get_text(s_tc_sf2_lon));

        // Finish line (A-B)
        if (!edit_td->is_circuit && s_tc_fin1_lat) {
            fill(s_tc_fin1_lat, edit_td->fin_lat1);
            fill(s_tc_fin1_lon, edit_td->fin_lon1);
            fill(s_tc_fin2_lat, edit_td->fin_lat2);
            fill(s_tc_fin2_lon, edit_td->fin_lon2);
        }

        // Sectors
        for (int i = 0; i < edit_td->sector_count && i < MAX_SECTORS; i++) {
            char sec_lbl[8]; snprintf(sec_lbl, sizeof(sec_lbl), "S%d", i+1);
            mk_coord_row(s_tc_sec_container, sec_lbl,
                         &s_tc_sec_lat[i], &s_tc_sec_lon[i]);
            fill(s_tc_sec_lat[i], edit_td->sectors[i].lat);
            fill(s_tc_sec_lon[i], edit_td->sectors[i].lon);
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

// Track filter state for history screen
static char s_hist_filter_track[48] = {};

static void open_history_screen() {
    lv_obj_t *scr = make_sub_screen(tr(TR_HISTORY_TITLE), cb_back_to_menu);
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    // ── Current session ───────────────────────────────────────────────────
    hist_section_header(content, tr(TR_HIST_CURRENT));

    // If the live reference currently points at a lap from a SAVED session
    // (manual pick from the detail screen, or the auto-loaded all-time best),
    // we must NOT highlight any current-session lap as "✓ REF".
    char ext_ref_sid[20] = {};
    uint8_t ext_ref_lap = 0;
    bool ext_ref_active = lap_timer_get_external_ref(ext_ref_sid,
                                                     sizeof(ext_ref_sid),
                                                     &ext_ref_lap);

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
            // PERF: flat corners on list rows — rounded corners with SW
            // renderer + many visible rows = expensive mask draw every
            // scroll frame (LVGL 9 + DRAW_UNIT_CNT=1). Visual diff minimal
            // inside a vertical list.
            lv_obj_set_style_radius(row, 0, LV_STATE_DEFAULT);
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
            bool is_cur_ref = !ext_ref_active && i == (int)sess.ref_lap_idx;
            brl_style_btn(ref_btn, is_cur_ref ? BRL_CLR_ACCENT : BRL_CLR_SURFACE2);
            lv_obj_t *rl2 = lv_label_create(ref_btn);
            lv_label_set_text(rl2, is_cur_ref ? tr(TR_IS_REF) : tr(TR_SET_REF));
            brl_style_label(rl2, &BRL_FONT_14, BRL_CLR_TEXT);
            lv_obj_center(rl2);
            lv_obj_set_user_data(ref_btn, (void*)(intptr_t)i);
            lv_obj_add_event_cb(ref_btn, [](lv_event_t *e){
                // Use current_target so we read the BUTTON's user_data,
                // not the inner label's (LVGL bubbles the click event,
                // so target may be the label which has no user_data → 0,
                // which made every "Set Ref" button select Lap 1).
                lv_obj_t *btn = (lv_obj_t*)lv_event_get_current_target(e);
                int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
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

        // ── Track filter dropdown ────────────────────────────────────────
        // Collect unique track names from summaries
        static char track_names[30][48];
        int n_tracks = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(s_summaries[i].id, sess.session_id) == 0) continue;
            if (strlen(s_summaries[i].track) == 0) continue;
            bool found = false;
            for (int t = 0; t < n_tracks; t++) {
                if (strcmp(track_names[t], s_summaries[i].track) == 0) { found = true; break; }
            }
            if (!found && n_tracks < 30) {
                strncpy(track_names[n_tracks], s_summaries[i].track, 47);
                track_names[n_tracks][47] = '\0';
                n_tracks++;
            }
        }

        if (n_tracks > 1) {
            static char dd_opts[512];
            dd_opts[0] = '\0';
            strncpy(dd_opts, tr(TR_HIST_ALL_TRACKS), sizeof(dd_opts) - 1);
            int active_opt = 0;
            for (int t = 0; t < n_tracks; t++) {
                strncat(dd_opts, "\n", sizeof(dd_opts) - strlen(dd_opts) - 1);
                strncat(dd_opts, track_names[t], sizeof(dd_opts) - strlen(dd_opts) - 1);
                if (s_hist_filter_track[0] != '\0' && strcmp(s_hist_filter_track, track_names[t]) == 0)
                    active_opt = t + 1;
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
                    s_hist_filter_track[0] = '\0';
                } else {
                    lv_dropdown_get_selected_str(obj, s_hist_filter_track,
                                                 sizeof(s_hist_filter_track));
                }
                open_history_screen();
            }, LV_EVENT_VALUE_CHANGED, nullptr);
        }

        // ── Session rows ─────────────────────────────────────────────────
        int shown = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(s_summaries[i].id, sess.session_id) == 0) continue;

            // Apply track filter
            if (s_hist_filter_track[0] != '\0' &&
                strcmp(s_summaries[i].track, s_hist_filter_track) != 0) continue;

            lv_obj_t *row = lv_obj_create(content);
            lv_obj_set_width(row, LV_PCT(100)); lv_obj_set_height(row, 60);
            brl_style_card(row);
            lv_obj_set_style_radius(row, 0, LV_STATE_DEFAULT); // PERF: flat for scroll
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            // Whole row is tappable → opens the lap-detail screen where the
            // user can pick a lap as live-delta reference. The delete button
            // below has its bubble disabled so that only the delete fires
            // when its button is tapped.
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(row, (void*)(intptr_t)i);
            lv_obj_add_event_cb(row, [](lv_event_t *e){
                int idx = (int)(intptr_t)lv_obj_get_user_data(
                    (lv_obj_t*)lv_event_get_current_target(e));
                open_session_detail_screen(s_summaries[idx].id,
                                           s_summaries[idx].name);
            }, LV_EVENT_CLICKED, nullptr);

            // Session name (top) + track as subtitle
            lv_obj_t *name_lbl = lv_label_create(row);
            lv_label_set_text(name_lbl, s_summaries[i].name);
            brl_style_label(name_lbl, &BRL_FONT_16, BRL_CLR_TEXT);
            lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

            lv_obj_t *id_lbl = lv_label_create(row);
            const char *sub = strlen(s_summaries[i].track) > 0
                              ? s_summaries[i].track : s_summaries[i].id;
            lv_label_set_text(id_lbl, sub);
            brl_style_label(id_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
            lv_obj_align(id_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

            // Right side: N laps + best lap + delete button
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
            lv_obj_align(info_lbl, LV_ALIGN_RIGHT_MID, -60, 0);

            // Delete button — must NOT bubble to the row handler, otherwise
            // tapping delete would both delete and try to open the (now
            // deleted) session's detail screen.
            lv_obj_t *del_btn = lv_button_create(row);
            lv_obj_set_size(del_btn, 48, 40);
            lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_remove_flag(del_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x5A1A1A), LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(del_btn, BRL_CLR_DANGER, LV_STATE_PRESSED);
            lv_obj_set_style_border_width(del_btn, 0, LV_STATE_DEFAULT);
            lv_obj_set_style_radius(del_btn, 6, LV_STATE_DEFAULT);
            lv_obj_set_style_shadow_width(del_btn, 0, LV_STATE_DEFAULT);
            lv_obj_t *del_lbl = lv_label_create(del_btn);
            lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
            brl_style_label(del_lbl, &BRL_FONT_16, lv_color_hex(0xFF6666));
            lv_obj_center(del_lbl);
            lv_obj_set_user_data(del_btn, (void*)(intptr_t)i);
            lv_obj_add_event_cb(del_btn, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
                session_store_delete_session(s_summaries[idx].id);
                open_history_screen();
            }, LV_EVENT_CLICKED, nullptr);

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

// ---------------------------------------------------------------------------
// Session-detail screen: lists the laps of a saved session and lets the user
// pick one as the live-delta reference. Reachable by tapping a row in the
// "Saved sessions" list on the history screen.
// ---------------------------------------------------------------------------
static char s_sd_session_id[20];
static char s_sd_session_name[64];

static void cb_back_to_history(lv_event_t * /*e*/) { open_history_screen(); }

static void open_session_detail_screen(const char *session_id,
                                       const char *session_name)
{
    // Snapshot args into file-static storage — the open() callback for the
    // Set-ref buttons rebuilds the screen and needs them to survive the
    // original caller's stack frame.
    strncpy(s_sd_session_id, session_id ? session_id : "",
            sizeof(s_sd_session_id) - 1);
    s_sd_session_id[sizeof(s_sd_session_id) - 1] = '\0';
    strncpy(s_sd_session_name, session_name ? session_name : "",
            sizeof(s_sd_session_name) - 1);
    s_sd_session_name[sizeof(s_sd_session_name) - 1] = '\0';

    lv_obj_t *scr = make_sub_screen(tr(TR_SESSION_LAPS_TITLE),
                                    cb_back_to_history);
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    // Session name as first line (so the user still sees which session they
    // opened even after the generic title).
    lv_obj_t *name_lbl = lv_label_create(content);
    lv_label_set_text(name_lbl, s_sd_session_name);
    brl_style_label(name_lbl, &BRL_FONT_16, BRL_CLR_ACCENT);

    static SessionLapInfo s_laps[MAX_LAPS_PER_SESSION];
    int n = session_store_list_laps(s_sd_session_id, s_laps,
                                    MAX_LAPS_PER_SESSION);

    if (n == 0) {
        lv_obj_t *ph = lv_label_create(content);
        lv_label_set_text(ph, tr(TR_NO_LAPS));
        brl_style_label(ph, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        sub_screen_load(scr);
        return;
    }

    // Is the currently-active live reference a lap FROM THIS session?
    char cur_ext_sid[20] = {};
    uint8_t cur_ext_lap = 255;
    bool ext_active = lap_timer_get_external_ref(cur_ext_sid,
                                                 sizeof(cur_ext_sid),
                                                 &cur_ext_lap);
    bool this_session_has_ref = ext_active &&
                                strcmp(cur_ext_sid, s_sd_session_id) == 0;

    // Find fastest lap in this session (for the "* BEST" badge). point_count
    // filter matches the "is it usable as reference" criterion below.
    int best_idx = -1;
    uint32_t best_ms = 0;
    for (int i = 0; i < n; i++) {
        if (s_laps[i].total_ms == 0) continue;
        if (s_laps[i].point_count == 0) continue;
        if (best_idx < 0 || s_laps[i].total_ms < best_ms) {
            best_idx = i;
            best_ms  = s_laps[i].total_ms;
        }
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 52);
        brl_style_card(row);
        lv_obj_set_style_radius(row, 0, LV_STATE_DEFAULT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char lt_buf[16]; fmt_laptime(lt_buf, sizeof(lt_buf), s_laps[i].total_ms);
        char lab[64];
        snprintf(lab, sizeof(lab), "%s %u    %s%s",
                 tr(TR_LAP), (unsigned)s_laps[i].lap_num, lt_buf,
                 (i == best_idx) ? "  * BEST" : "");

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, lab);
        brl_style_label(lbl, &BRL_FONT_16,
                        (i == best_idx) ? BRL_CLR_ACCENT : BRL_CLR_TEXT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        if (s_laps[i].point_count == 0) {
            // No GPS data → cannot be used as live-delta reference. Show a
            // muted note instead of a button so the user understands why.
            lv_obj_t *no_gps = lv_label_create(row);
            lv_label_set_text(no_gps, tr(TR_LAP_NO_GPS));
            brl_style_label(no_gps, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
            lv_obj_align(no_gps, LV_ALIGN_RIGHT_MID, 0, 0);
            continue;
        }

        lv_obj_t *ref_btn = lv_button_create(row);
        lv_obj_set_size(ref_btn, 130, 34);
        lv_obj_align(ref_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        bool is_ref = this_session_has_ref && cur_ext_lap == (uint8_t)i;
        brl_style_btn(ref_btn, is_ref ? BRL_CLR_ACCENT : BRL_CLR_SURFACE2);
        lv_obj_t *rlbl = lv_label_create(ref_btn);
        lv_label_set_text(rlbl, is_ref ? tr(TR_IS_REF) : tr(TR_SET_REF));
        brl_style_label(rlbl, &BRL_FONT_14, BRL_CLR_TEXT);
        lv_obj_center(rlbl);
        lv_obj_set_user_data(ref_btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(ref_btn, [](lv_event_t *e){
            lv_obj_t *btn = (lv_obj_t*)lv_event_get_current_target(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
            lap_timer_set_ref_from_saved(s_sd_session_id, (uint8_t)idx);
            open_session_detail_screen(s_sd_session_id, s_sd_session_name);
        }, LV_EVENT_CLICKED, nullptr);
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
    brl_style_label(ico, &BRL_FONT_24, BRL_CLR_ACCENT);
    lv_obj_align(ico, LV_ALIGN_LEFT_MID, 0, subtitle ? -8 : 0);

    lv_obj_t *tit = lv_label_create(row);
    lv_label_set_text(tit, title);
    brl_style_label(tit, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_align(tit, LV_ALIGN_LEFT_MID, 36, subtitle ? -8 : 0);

    if (subtitle) {
        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text(sub, subtitle);
        brl_style_label(sub, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 36, 12);
    }

    lv_obj_t *right = lv_obj_create(row);
    lv_obj_set_size(right, 420, h - 16);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    brl_style_transparent(right);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    return right;
}

static lv_obj_t *make_setting_btn(lv_obj_t *parent, const char *text,
                                   lv_color_t color, lv_align_t align, int x_ofs = 0) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_align(btn, align, x_ofs, 0);
    lv_obj_set_style_bg_color(btn, color, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    brl_style_label(lbl, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_center(lbl);
    return btn;
}

// WiFi AP config dialog
static void cb_ap_dialog_save(lv_event_t* /*e*/) {
    if (s_ap_ta_ssid && s_ap_ta_pass) {
        wifi_ap_set_config(lv_textarea_get_text(s_ap_ta_ssid),
                           lv_textarea_get_text(s_ap_ta_pass));
    }
    if (s_ap_dialog) { lv_obj_delete(s_ap_dialog); s_ap_dialog = nullptr; }
    s_ap_ta_ssid = s_ap_ta_pass = s_ap_kb = nullptr;
}
static void cb_ap_dialog_cancel(lv_event_t* /*e*/) {
    if (s_ap_dialog) { lv_obj_delete(s_ap_dialog); s_ap_dialog = nullptr; }
    s_ap_ta_ssid = s_ap_ta_pass = s_ap_kb = nullptr;
}
static void cb_ap_ta_focus(lv_event_t *e) {
    if (s_ap_kb) {
        lv_keyboard_set_textarea(s_ap_kb, (lv_obj_t*)lv_event_get_target(e));
        lv_obj_remove_flag(s_ap_kb, LV_OBJ_FLAG_HIDDEN);
    }
}
static void open_wifi_ap_dialog() {
    if (s_ap_dialog) return;
    lv_obj_t *scr = lv_screen_active();

    // Full-screen overlay
    s_ap_dialog = lv_obj_create(scr);
    lv_obj_set_size(s_ap_dialog, BRL_SCREEN_W, BRL_SCREEN_H); lv_obj_set_pos(s_ap_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_ap_dialog, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_ap_dialog, LV_OPA_80, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_ap_dialog, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_ap_dialog, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_ap_dialog, LV_OBJ_FLAG_SCROLLABLE);

    // Card (top strip, above keyboard)
    lv_obj_t *card = lv_obj_create(s_ap_dialog);
    lv_obj_set_size(card, BRL_SCREEN_W, 270); lv_obj_set_pos(card, 0, 40);
    brl_style_card(card); lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 16, LV_STATE_DEFAULT);

    // Title
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Hotspot konfigurieren");
    brl_style_label(title, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_set_pos(title, 0, 0);

    // IP info row
    lv_obj_t *ip_lbl = lv_label_create(card);
    lv_label_set_text_fmt(ip_lbl, "IP-Adresse: %s", wifi_ap_ip());
    brl_style_label(ip_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(ip_lbl, 0, 28);

    // SSID field
    lv_obj_t *ssid_lbl = lv_label_create(card);
    lv_label_set_text(ssid_lbl, "Hotspot-Name (SSID):");
    brl_style_label(ssid_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(ssid_lbl, 0, 56);

    s_ap_ta_ssid = lv_textarea_create(card);
    lv_obj_set_size(s_ap_ta_ssid, LV_PCT(100), 44);
    lv_obj_set_pos(s_ap_ta_ssid, 0, 76);
    lv_textarea_set_one_line(s_ap_ta_ssid, true);
    lv_textarea_set_max_length(s_ap_ta_ssid, 31);
    lv_textarea_set_text(s_ap_ta_ssid, wifi_ap_ssid());
    lv_obj_set_style_text_font(s_ap_ta_ssid, &BRL_FONT_16, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_ap_ta_ssid, lv_color_hex(0x111111), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_ap_ta_ssid, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_ap_ta_ssid, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_ap_ta_ssid, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_ap_ta_ssid, cb_ap_ta_focus, LV_EVENT_FOCUSED, nullptr);

    // Password field
    lv_obj_t *pass_lbl = lv_label_create(card);
    lv_label_set_text(pass_lbl, "Passwort (leer = offen):");
    brl_style_label(pass_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(pass_lbl, 0, 130);

    s_ap_ta_pass = lv_textarea_create(card);
    lv_obj_set_size(s_ap_ta_pass, LV_PCT(100), 44);
    lv_obj_set_pos(s_ap_ta_pass, 0, 150);
    lv_textarea_set_one_line(s_ap_ta_pass, true);
    lv_textarea_set_max_length(s_ap_ta_pass, 63);
    lv_textarea_set_password_mode(s_ap_ta_pass, true);
    lv_textarea_set_text(s_ap_ta_pass, wifi_ap_pass());
    lv_obj_set_style_text_font(s_ap_ta_pass, &BRL_FONT_16, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_ap_ta_pass, lv_color_hex(0x111111), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_ap_ta_pass, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_ap_ta_pass, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_ap_ta_pass, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_ap_ta_pass, cb_ap_ta_focus, LV_EVENT_FOCUSED, nullptr);

    // Buttons
    auto mk_btn = [&](const char *lbl, lv_color_t col, int x_ofs) -> lv_obj_t* {
        lv_obj_t *b = lv_button_create(card);
        lv_obj_set_size(b, 220, 40);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, x_ofs, 0);
        lv_obj_set_style_bg_color(b, col, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(b, 8, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(b, 0, LV_STATE_DEFAULT);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, lbl);
        brl_style_label(l, &BRL_FONT_16, BRL_CLR_TEXT);
        lv_obj_center(l);
        return b;
    };
    char save_lbl[32], cancel_lbl[32];
    snprintf(save_lbl,   sizeof(save_lbl),   LV_SYMBOL_OK    "  %s", tr(TR_SAVE_BTN));
    snprintf(cancel_lbl, sizeof(cancel_lbl), LV_SYMBOL_CLOSE "  %s", tr(TR_CANCEL_BTN));
    lv_obj_t *bsave   = mk_btn(save_lbl,   BRL_CLR_ACCENT,    0);
    lv_obj_t *bcancel = mk_btn(cancel_lbl, BRL_CLR_SURFACE2, -240);
    lv_obj_add_event_cb(bsave,   cb_ap_dialog_save,   LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(bcancel, cb_ap_dialog_cancel, LV_EVENT_CLICKED, nullptr);

    // Keyboard at overlay bottom
    s_ap_kb = lv_keyboard_create(s_ap_dialog);
    lv_keyboard_set_map(s_ap_kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_qwertz_lc, s_qwertz_ctrl);
    lv_keyboard_set_map(s_ap_kb, LV_KEYBOARD_MODE_TEXT_UPPER, s_qwertz_uc, s_qwertz_ctrl);
    lv_obj_set_size(s_ap_kb, BRL_SCREEN_W, 266);
    lv_obj_align(s_ap_kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_ap_kb, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_ap_kb, [](lv_event_t *e){
        lv_obj_add_flag((lv_obj_t*)lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(s_ap_kb, LV_OBJ_FLAG_HIDDEN);
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
// Scan results storage (shared between scan and dialog)
static WifiScanResult s_scan_results[20];
static int s_scan_count = 0;

// Show the password-entry dialog for a specific SSID
static void open_wifi_pass_dialog(const char *ssid);

static void open_wifi_sta_dialog() {
    if (s_wifi_dialog) return;
    lv_obj_t *scr = lv_screen_active();
    s_wifi_dialog = lv_obj_create(scr);
    lv_obj_set_size(s_wifi_dialog, BRL_SCREEN_W, BRL_SCREEN_H);
    lv_obj_set_pos(s_wifi_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_dialog, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_wifi_dialog, LV_OPA_80, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_wifi_dialog, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);

    // Card — scan results list
    lv_obj_t *card = lv_obj_create(s_wifi_dialog);
    lv_obj_set_size(card, 560, 440);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    brl_style_card(card);
    lv_obj_set_style_pad_all(card, 16, LV_STATE_DEFAULT);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text_fmt(ttl, LV_SYMBOL_WIFI "  %s", tr(TR_WIFI_DLG_TITLE));
    brl_style_label(ttl, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Scrollable list area
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_size(list, 528, 310);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 34);
    brl_style_transparent(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 4, LV_STATE_DEFAULT);

    // Scan for networks
    lv_obj_t *scanning_lbl = lv_label_create(list);
    lv_label_set_text(scanning_lbl, tr(TR_WIFI_SCANNING));
    brl_style_label(scanning_lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
    lv_refr_now(nullptr); // force display update before blocking scan

    s_scan_count = wifi_scan_start();
    if (s_scan_count > 0) {
        s_scan_count = wifi_scan_get_results(s_scan_results, 20);
    }
    lv_obj_delete(scanning_lbl);

    if (s_scan_count == 0) {
        lv_obj_t *no_net = lv_label_create(list);
        lv_label_set_text(no_net, tr(TR_WIFI_NO_NETWORKS));
        brl_style_label(no_net, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
    } else {
        for (int i = 0; i < s_scan_count; i++) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 48);
            lv_obj_set_style_bg_color(row, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(row, BRL_CLR_ACCENT_DIM, LV_STATE_PRESSED);
            lv_obj_set_style_border_width(row, 0, LV_STATE_DEFAULT);
            lv_obj_set_style_radius(row, 6, LV_STATE_DEFAULT);
            lv_obj_set_style_pad_hor(row, 10, LV_STATE_DEFAULT);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

            // SSID name
            lv_obj_t *name = lv_label_create(row);
            lv_label_set_text(name, s_scan_results[i].ssid);
            brl_style_label(name, &BRL_FONT_16, BRL_CLR_TEXT);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

            // RSSI + lock icon
            char info[24];
            snprintf(info, sizeof(info), "%s %d dBm",
                     s_scan_results[i].authmode ? LV_SYMBOL_SETTINGS : "",
                     (int)s_scan_results[i].rssi);
            lv_obj_t *rssi_lbl = lv_label_create(row);
            lv_label_set_text(rssi_lbl, info);
            brl_style_label(rssi_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
            lv_obj_align(rssi_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

            // Click → password dialog
            lv_obj_set_user_data(row, (void*)(intptr_t)i);
            lv_obj_add_event_cb(row, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_obj_get_user_data(
                    (lv_obj_t*)lv_event_get_target(e));
                if (idx >= 0 && idx < s_scan_count) {
                    // Close scan dialog first
                    if (s_wifi_dialog) {
                        lv_obj_delete(s_wifi_dialog);
                        s_wifi_dialog = nullptr;
                    }
                    open_wifi_pass_dialog(s_scan_results[idx].ssid);
                }
            }, LV_EVENT_CLICKED, nullptr);
        }
    }

    // Bottom buttons: Manual + Cancel
    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_set_size(btn_row, 528, 44);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    brl_style_transparent(btn_row);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    // Manual entry button
    lv_obj_t *bmanual = lv_button_create(btn_row);
    lv_obj_set_size(bmanual, 200, 40);
    lv_obj_align(bmanual, LV_ALIGN_LEFT_MID, 0, 0);
    brl_style_btn(bmanual, BRL_CLR_SURFACE2);
    lv_obj_t *ml = lv_label_create(bmanual);
    lv_label_set_text_fmt(ml, LV_SYMBOL_EDIT "  %s", tr(TR_WIFI_MANUAL));
    brl_style_label(ml, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(ml);
    lv_obj_add_event_cb(bmanual, [](lv_event_t* /*e*/) {
        if (s_wifi_dialog) { lv_obj_delete(s_wifi_dialog); s_wifi_dialog = nullptr; }
        open_wifi_pass_dialog("");
    }, LV_EVENT_CLICKED, nullptr);

    // Cancel button
    lv_obj_t *bcancel = lv_button_create(btn_row);
    lv_obj_set_size(bcancel, 140, 40);
    lv_obj_align(bcancel, LV_ALIGN_RIGHT_MID, 0, 0);
    brl_style_btn(bcancel, BRL_CLR_SURFACE2);
    lv_obj_t *cl = lv_label_create(bcancel);
    char cancel_buf[48];
    snprintf(cancel_buf, sizeof(cancel_buf), LV_SYMBOL_CLOSE "  %s", tr(TR_CANCEL_DLG));
    lv_label_set_text(cl, cancel_buf);
    brl_style_label(cl, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(cl);
    lv_obj_add_event_cb(bcancel, cb_wifi_dialog_cancel, LV_EVENT_CLICKED, nullptr);
}

// Password entry dialog (after selecting SSID from scan or manual)
static void open_wifi_pass_dialog(const char *ssid) {
    if (s_wifi_dialog) return;
    lv_obj_t *scr = lv_screen_active();

    bool manual = (ssid == nullptr || strlen(ssid) == 0);

    s_wifi_dialog = lv_obj_create(scr);
    lv_obj_set_size(s_wifi_dialog, BRL_SCREEN_W, BRL_SCREEN_H);
    lv_obj_set_pos(s_wifi_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_dialog, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_wifi_dialog, LV_OPA_80, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_wifi_dialog, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_wifi_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_wifi_dialog);
    lv_obj_set_size(card, 500, manual ? 300 : 240);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    brl_style_card(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(card, 16, LV_STATE_DEFAULT);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text_fmt(ttl, LV_SYMBOL_WIFI "  %s", tr(TR_WIFI_DLG_TITLE));
    brl_style_label(ttl, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    int y = 36;

    // SSID field — editable if manual, pre-filled if from scan
    if (manual) {
        lv_obj_t *l1 = lv_label_create(card);
        lv_label_set_text(l1, tr(TR_SSID_LABEL));
        brl_style_label(l1, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, y);
        y += 24;
        s_ta_ssid = lv_textarea_create(card);
        lv_obj_set_size(s_ta_ssid, 468, 40);
        lv_obj_align(s_ta_ssid, LV_ALIGN_TOP_LEFT, 0, y);
        lv_textarea_set_one_line(s_ta_ssid, true);
        lv_textarea_set_placeholder_text(s_ta_ssid, "SSID");
        lv_obj_set_style_bg_color(s_ta_ssid, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_ta_ssid, BRL_CLR_TEXT, LV_STATE_DEFAULT);
        lv_obj_add_event_cb(s_ta_ssid, cb_dlg_ta_focus, LV_EVENT_FOCUSED, nullptr);
        y += 50;
    } else {
        // Show selected SSID as label
        lv_obj_t *ssid_lbl = lv_label_create(card);
        lv_label_set_text_fmt(ssid_lbl, "%s: %s", tr(TR_SSID_LABEL), ssid);
        brl_style_label(ssid_lbl, &BRL_FONT_16, BRL_CLR_ACCENT);
        lv_obj_align(ssid_lbl, LV_ALIGN_TOP_LEFT, 0, y);
        // Store SSID in a hidden textarea so cb_wifi_dialog_save can read it
        s_ta_ssid = lv_textarea_create(card);
        lv_textarea_set_text(s_ta_ssid, ssid);
        lv_obj_add_flag(s_ta_ssid, LV_OBJ_FLAG_HIDDEN);
        y += 30;
    }

    // Password field
    lv_obj_t *l2 = lv_label_create(card);
    lv_label_set_text(l2, tr(TR_PASS_LABEL));
    brl_style_label(l2, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, y);
    y += 24;

    s_ta_pass = lv_textarea_create(card);
    lv_obj_set_size(s_ta_pass, 468, 40);
    lv_obj_align(s_ta_pass, LV_ALIGN_TOP_LEFT, 0, y);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "••••••••");
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_obj_set_style_bg_color(s_ta_pass, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_ta_pass, BRL_CLR_TEXT, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_ta_pass, cb_dlg_ta_focus, LV_EVENT_FOCUSED, nullptr);

    // Buttons
    char dlg_conn[48], dlg_cancel[48];
    snprintf(dlg_conn,   sizeof(dlg_conn),   LV_SYMBOL_OK    "  %s", tr(TR_CONNECT_DLG));
    snprintf(dlg_cancel, sizeof(dlg_cancel), LV_SYMBOL_CLOSE "  %s", tr(TR_CANCEL_DLG));

    lv_obj_t *bsave = lv_button_create(card);
    lv_obj_set_size(bsave, 140, 40);
    lv_obj_align(bsave, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    brl_style_btn(bsave, BRL_CLR_ACCENT);
    lv_obj_t *sl = lv_label_create(bsave);
    lv_label_set_text(sl, dlg_conn);
    brl_style_label(sl, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(sl);
    lv_obj_add_event_cb(bsave, cb_wifi_dialog_save, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *bcancel = lv_button_create(card);
    lv_obj_set_size(bcancel, 140, 40);
    lv_obj_align(bcancel, LV_ALIGN_BOTTOM_RIGHT, -150, 0);
    brl_style_btn(bcancel, BRL_CLR_SURFACE2);
    lv_obj_t *cl2 = lv_label_create(bcancel);
    lv_label_set_text(cl2, dlg_cancel);
    brl_style_label(cl2, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(cl2);
    lv_obj_add_event_cb(bcancel, cb_wifi_dialog_cancel, LV_EVENT_CLICKED, nullptr);

    // Keyboard
    s_dlg_kb = lv_keyboard_create(s_wifi_dialog);
    lv_keyboard_set_map(s_dlg_kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_qwertz_lc, s_qwertz_ctrl);
    lv_keyboard_set_map(s_dlg_kb, LV_KEYBOARD_MODE_TEXT_UPPER, s_qwertz_uc, s_qwertz_ctrl);
    lv_obj_set_size(s_dlg_kb, BRL_SCREEN_W, 266);
    lv_obj_align(s_dlg_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_dlg_kb, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_dlg_kb, cb_dlg_kb_ready, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(s_dlg_kb, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// GPS INFO SCREEN
// ============================================================================
static lv_timer_t *s_gps_timer = nullptr;

static void open_gps_info_screen() {
    lv_obj_t *scr = make_sub_screen("GPS Info", [](lv_event_t* /*e*/) {
        if (s_gps_timer) { lv_timer_delete(s_gps_timer); s_gps_timer = nullptr; }
        open_settings_screen();
    });
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    // Helper: create a labeled info row
    struct InfoRow { lv_obj_t *val_lbl; };
    static InfoRow rows[10];
    int ri = 0;

    auto add_row = [&](const char *label) -> lv_obj_t* {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 44);
        brl_style_card(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, label);
        brl_style_label(lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, "---");
        brl_style_label(val, &BRL_FONT_20, BRL_CLR_TEXT);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
        rows[ri++].val_lbl = val;
        return val;
    };

    add_row(tr(TR_GPS_STATUS));    // 0
    add_row(tr(TR_GPS_SATS));      // 1
    add_row(tr(TR_GPS_HDOP));      // 2
    add_row(tr(TR_GPS_COORDS));    // 3
    add_row(tr(TR_GPS_ALTITUDE));  // 4
    add_row(tr(TR_GPS_SPEED));     // 5
    add_row(tr(TR_GPS_HEADING));   // 6
    add_row(tr(TR_GPS_TIME));      // 7
    add_row(tr(TR_GPS_PPS));       // 8
    add_row("Update Rate");        // 9

    // Update timer (500ms)
    s_gps_timer = lv_timer_create([](lv_timer_t *t) {
        (void)t;
        char buf[64];
        const GpsData &g = g_state.gps;
        GpsDateTime dt = gps_get_datetime();

        // Status
        if (g.valid) {
            lv_label_set_text(rows[0].val_lbl, tr(TR_GPS_FIX_OK));
            brl_style_label(rows[0].val_lbl, &BRL_FONT_20, lv_color_hex(0x00CC66));
        } else {
            lv_label_set_text(rows[0].val_lbl, tr(TR_GPS_NO_FIX));
            brl_style_label(rows[0].val_lbl, &BRL_FONT_20, BRL_CLR_DANGER);
        }

        // Satellites
        snprintf(buf, sizeof(buf), "%d", g.satellites);
        lv_label_set_text(rows[1].val_lbl, buf);
        lv_color_t sat_clr = g.satellites >= 6 ? lv_color_hex(0x00CC66) :
                             g.satellites >= 3 ? lv_color_hex(0xFFAA00) : BRL_CLR_DANGER;
        brl_style_label(rows[1].val_lbl, &BRL_FONT_20, sat_clr);

        // HDOP
        snprintf(buf, sizeof(buf), "%.1f", (double)g.hdop);
        lv_label_set_text(rows[2].val_lbl, buf);
        lv_color_t hdop_clr = g.hdop < 2.0f ? lv_color_hex(0x00CC66) :
                              g.hdop < 5.0f ? lv_color_hex(0xFFAA00) : BRL_CLR_DANGER;
        brl_style_label(rows[2].val_lbl, &BRL_FONT_20, hdop_clr);

        // Coordinates
        if (g.valid) {
            snprintf(buf, sizeof(buf), "%.6f, %.6f", g.lat, g.lon);
        } else {
            snprintf(buf, sizeof(buf), "---");
        }
        lv_label_set_text(rows[3].val_lbl, buf);

        // Altitude
        snprintf(buf, sizeof(buf), "%.1f m", (double)g.altitude_m);
        lv_label_set_text(rows[4].val_lbl, buf);

        // Speed
        snprintf(buf, sizeof(buf), "%.1f km/h", (double)g.speed_kmh);
        lv_label_set_text(rows[5].val_lbl, buf);

        // Heading
        snprintf(buf, sizeof(buf), "%.1f°", (double)g.heading_deg);
        lv_label_set_text(rows[6].val_lbl, buf);

        // UTC Time
        if (dt.valid) {
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d  %02d.%02d.%04d",
                     dt.hour, dt.minute, dt.second, dt.day, dt.month, dt.year);
        } else {
            snprintf(buf, sizeof(buf), "---");
        }
        lv_label_set_text(rows[7].val_lbl, buf);

        // PPS (read GPIO level)
        int pps = gpio_get_level((gpio_num_t)GPS_PPS_PIN);
        lv_label_set_text(rows[8].val_lbl, pps ? "HIGH" : "LOW");
        brl_style_label(rows[8].val_lbl, &BRL_FONT_20,
                        pps ? lv_color_hex(0x00CC66) : BRL_CLR_TEXT_DIM);

        // Update rate (measured Hz)
        uint8_t hz = gps_get_update_rate();
        snprintf(buf, sizeof(buf), "%d Hz", hz);
        lv_label_set_text(rows[9].val_lbl, buf);
        lv_color_t hz_clr = hz >= 9 ? lv_color_hex(0x00CC66) :
                            hz >= 4 ? lv_color_hex(0xFFAA00) : BRL_CLR_DANGER;
        brl_style_label(rows[9].val_lbl, &BRL_FONT_20, hz_clr);

    }, 500, nullptr);

    sub_screen_load(scr);
}

// ============================================================================
// ANALOG INPUT CALIBRATION SCREEN  (AN1..AN4 on GPIO 20/21/22/23)
// ============================================================================
//
// One scrollable card per channel with:
//   - live mV + scaled value (refreshed by a 100 ms lv_timer)
//   - enabled switch (top-right)
//   - text inputs for name + scale + offset + min + max (numeric ones use
//     the LVGL number-mode keyboard; the name field uses the text keyboard)
//   - a single shared on-screen keyboard at the bottom of the screen
//
// The "Speichern" action button in the sub-header reads all 24 fields back
// into g_analog_cfg and calls analog_in_save_config() (NVS commit), then
// returns to the settings screen.
// ============================================================================
static lv_obj_t   *s_an_kb           = nullptr;
static lv_obj_t   *s_an_ta_name[ANALOG_CHANNELS]   = {};
static lv_obj_t   *s_an_ta_scale[ANALOG_CHANNELS]  = {};
static lv_obj_t   *s_an_ta_offset[ANALOG_CHANNELS] = {};
static lv_obj_t   *s_an_ta_min[ANALOG_CHANNELS]    = {};
static lv_obj_t   *s_an_ta_max[ANALOG_CHANNELS]    = {};
static lv_obj_t   *s_an_sw[ANALOG_CHANNELS]        = {};
static lv_obj_t   *s_an_live[ANALOG_CHANNELS]      = {};
static lv_timer_t *s_an_refresh_timer              = nullptr;

static void an_clear_state(void) {
    if (s_an_refresh_timer) {
        lv_timer_delete(s_an_refresh_timer);
        s_an_refresh_timer = nullptr;
    }
    s_an_kb = nullptr;
    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        s_an_ta_name[i] = s_an_ta_scale[i] = s_an_ta_offset[i] = nullptr;
        s_an_ta_min[i] = s_an_ta_max[i] = s_an_sw[i] = s_an_live[i] = nullptr;
    }
}

static void an_ta_focus_cb(lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
    if (!s_an_kb || !ta) return;
    bool numeric = lv_obj_get_user_data(ta) != nullptr;
    lv_keyboard_set_mode(s_an_kb,
                         numeric ? LV_KEYBOARD_MODE_NUMBER
                                 : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_an_kb, ta);
    lv_obj_remove_flag(s_an_kb, LV_OBJ_FLAG_HIDDEN);
}

static void an_kb_done_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if ((c == LV_EVENT_READY || c == LV_EVENT_CANCEL) && s_an_kb) {
        lv_obj_add_flag(s_an_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void an_save_cb(lv_event_t * /*e*/) {
    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        if (s_an_ta_name[i]) {
            const char *t = lv_textarea_get_text(s_an_ta_name[i]);
            strncpy(g_analog_cfg[i].name, t, sizeof(g_analog_cfg[i].name) - 1);
            g_analog_cfg[i].name[sizeof(g_analog_cfg[i].name) - 1] = '\0';
        }
        if (s_an_ta_scale[i])
            g_analog_cfg[i].scale   = (float)atof(lv_textarea_get_text(s_an_ta_scale[i]));
        if (s_an_ta_offset[i])
            g_analog_cfg[i].offset  = (float)atof(lv_textarea_get_text(s_an_ta_offset[i]));
        if (s_an_ta_min[i])
            g_analog_cfg[i].min_val = (float)atof(lv_textarea_get_text(s_an_ta_min[i]));
        if (s_an_ta_max[i])
            g_analog_cfg[i].max_val = (float)atof(lv_textarea_get_text(s_an_ta_max[i]));
        if (s_an_sw[i])
            g_analog_cfg[i].enabled = lv_obj_has_state(s_an_sw[i], LV_STATE_CHECKED);
    }
    analog_in_save_config();
    an_clear_state();
    open_settings_screen();
}

static void an_back_cb(lv_event_t * /*e*/) {
    an_clear_state();
    open_settings_screen();
}

static void an_refresh_cb(lv_timer_t * /*t*/) {
    char buf[40];
    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        if (!s_an_live[i]) continue;
        const AnalogChannel &a = g_state.analog[i];
        if (!a.valid) {
            lv_label_set_text(s_an_live[i], "—");
        } else {
            snprintf(buf, sizeof(buf), "%d mV  →  %.2f",
                     (int)a.raw_mv, a.value);
            lv_label_set_text(s_an_live[i], buf);
        }
    }
}

static void open_analog_screen() {
    an_clear_state();
    lv_obj_t *action_btn = nullptr;
    lv_obj_t *scr = make_sub_screen(tr(TR_ANALOG_TITLE), an_back_cb,
                                    &action_btn, tr(TR_SAVE));
    if (action_btn)
        lv_obj_add_event_cb(action_btn, an_save_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, LV_STATE_DEFAULT);
    // Leave room at the bottom so the keyboard doesn't cover the last card
    lv_obj_set_style_pad_bottom(content, 220, LV_STATE_DEFAULT);

    char buf[32];
    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        lv_obj_t *card = lv_obj_create(content);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, 120);
        brl_style_card(card);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Header — channel id (left), live value (center), enable switch (right)
        snprintf(buf, sizeof(buf), "AN%d", i + 1);
        lv_obj_t *hdr = lv_label_create(card);
        lv_label_set_text(hdr, buf);
        brl_style_label(hdr, &BRL_FONT_24, BRL_CLR_ACCENT);
        lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 8, 6);

        s_an_live[i] = lv_label_create(card);
        lv_label_set_text(s_an_live[i], "—");
        brl_style_label(s_an_live[i], &BRL_FONT_20, BRL_CLR_TEXT);
        lv_obj_align(s_an_live[i], LV_ALIGN_TOP_MID, 0, 8);

        s_an_sw[i] = lv_switch_create(card);
        lv_obj_set_size(s_an_sw[i], 60, 32);
        lv_obj_align(s_an_sw[i], LV_ALIGN_TOP_RIGHT, -8, 6);
        if (g_analog_cfg[i].enabled) lv_obj_add_state(s_an_sw[i], LV_STATE_CHECKED);

        // Helper: label + textarea pair, positioned absolutely inside the card
        auto mk_field = [&](const char *label, int x, int y, int w,
                            const char *initial, bool numeric) -> lv_obj_t* {
            lv_obj_t *l = lv_label_create(card);
            lv_label_set_text(l, label);
            brl_style_label(l, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
            lv_obj_set_pos(l, x, y);

            lv_obj_t *ta = lv_textarea_create(card);
            lv_obj_set_size(ta, w, 32);
            lv_obj_set_pos(ta, x + 70, y - 6);
            lv_textarea_set_one_line(ta, true);
            lv_textarea_set_text(ta, initial);
            lv_textarea_set_max_length(ta, 15);
            lv_obj_set_user_data(ta, numeric ? (void*)1 : nullptr);
            lv_obj_set_style_text_font(ta, &BRL_FONT_14, LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(ta, 4, LV_STATE_DEFAULT);
            lv_obj_add_event_cb(ta, an_ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
            return ta;
        };

        // Row 1 (y=54): Name | Scale | Offset
        snprintf(buf, sizeof(buf), "%s", g_analog_cfg[i].name);
        s_an_ta_name[i] = mk_field(tr(TR_ANALOG_NAME), 8, 54, 120, buf, false);
        snprintf(buf, sizeof(buf), "%g", (double)g_analog_cfg[i].scale);
        s_an_ta_scale[i] = mk_field(tr(TR_ANALOG_SCALE), 268, 54, 110, buf, true);
        snprintf(buf, sizeof(buf), "%g", (double)g_analog_cfg[i].offset);
        s_an_ta_offset[i] = mk_field(tr(TR_ANALOG_OFFSET), 528, 54, 110, buf, true);

        // Row 2 (y=88): Min | Max
        snprintf(buf, sizeof(buf), "%g", (double)g_analog_cfg[i].min_val);
        s_an_ta_min[i] = mk_field(tr(TR_ANALOG_MIN), 268, 88, 110, buf, true);
        snprintf(buf, sizeof(buf), "%g", (double)g_analog_cfg[i].max_val);
        s_an_ta_max[i] = mk_field(tr(TR_ANALOG_MAX), 528, 88, 110, buf, true);
    }

    // Single shared on-screen keyboard at the bottom — hidden until a TA is
    // focused. The user dismisses it with the Tick (READY) or X (CANCEL).
    s_an_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_an_kb, BRL_SCREEN_W, 200);
    lv_obj_align(s_an_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_an_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_an_kb, an_kb_done_cb, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(s_an_kb, an_kb_done_cb, LV_EVENT_CANCEL, nullptr);

    s_an_refresh_timer = lv_timer_create(an_refresh_cb, 100, nullptr);

    sub_screen_load(scr);
}

// ============================================================================
// CUSTOM CAN CHANNEL EDITOR  (RaceCapture-style: list + per-sensor editor)
// ============================================================================
//
// Two screens:
//   open_can_channels_screen()  — scrollable list of every sensor in the
//     active car profile. "+ Neuer Kanal" opens an empty editor; tapping a
//     row opens the editor pre-filled.
//   open_can_channel_edit(idx)  — form with name, CAN-ID (hex), proto,
//     start byte, length, unsigned flag, scale, offset, min, max. Save
//     writes back into g_car_profile and persists the active .brl on SD
//     via car_profile_save(). Delete drops the sensor + saves. Cancel
//     returns without writing.
//
// Active profile filename is read once from NVS via car_profile_get_active().
// ============================================================================

static int           s_ce_edit_idx     = -1;     // -1 = new sensor mode
static lv_obj_t     *s_ce_kb           = nullptr;
static lv_obj_t     *s_ce_ta_name      = nullptr;
static lv_obj_t     *s_ce_ta_canid     = nullptr;
static lv_obj_t     *s_ce_dd_proto     = nullptr;
static lv_obj_t     *s_ce_ta_start     = nullptr;
static lv_obj_t     *s_ce_ta_len       = nullptr;
static lv_obj_t     *s_ce_sw_unsigned  = nullptr;
static lv_obj_t     *s_ce_ta_scale     = nullptr;
static lv_obj_t     *s_ce_ta_offset    = nullptr;
static lv_obj_t     *s_ce_ta_min       = nullptr;
static lv_obj_t     *s_ce_ta_max       = nullptr;

static const uint8_t CAN_PROTO_VALUES[] = { 0, 7, 1 };
static const char *  CAN_PROTO_LABELS  = "PT-CAN Broadcast\nOBD2 (7DF)\nBMW UDS";

static void ce_clear_state(void) {
    s_ce_kb = s_ce_ta_name = s_ce_ta_canid = s_ce_dd_proto = nullptr;
    s_ce_ta_start = s_ce_ta_len = s_ce_sw_unsigned = nullptr;
    s_ce_ta_scale = s_ce_ta_offset = s_ce_ta_min = s_ce_ta_max = nullptr;
}

static void ce_ta_focus_cb(lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t*)lv_event_get_target(e);
    if (!s_ce_kb || !ta) return;
    bool numeric = lv_obj_get_user_data(ta) != nullptr;
    lv_keyboard_set_mode(s_ce_kb,
                         numeric ? LV_KEYBOARD_MODE_NUMBER
                                 : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_ce_kb, ta);
    lv_obj_remove_flag(s_ce_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ce_kb_done_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if ((c == LV_EVENT_READY || c == LV_EVENT_CANCEL) && s_ce_kb)
        lv_obj_add_flag(s_ce_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ce_show_save_fail(void) {
    lv_obj_t *s = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s, 600, 100);
    lv_obj_center(s);
    brl_style_card(s);
    lv_obj_set_style_bg_color(s, BRL_CLR_DANGER, LV_STATE_DEFAULT);
    lv_obj_t *l = lv_label_create(s);
    lv_label_set_text(l, tr(TR_CAN_SAVE_FAIL));
    brl_style_label(l, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_center(l);
    lv_timer_t *t = lv_timer_create([](lv_timer_t *t){
        lv_obj_t *o = (lv_obj_t*)lv_timer_get_user_data(t);
        if (o) lv_obj_delete(o);
        lv_timer_delete(t);
    }, 2000, nullptr);
    lv_timer_set_user_data(t, s);
}

static bool ce_persist_active_profile(void) {
    char fn[CAR_NAME_LEN];
    car_profile_get_active(fn, sizeof(fn));
    if (!fn[0]) return false;
    return car_profile_save(fn);
}

// --- Editor save callback ---------------------------------------------------
static void ce_save_cb(lv_event_t * /*e*/) {
    int idx = s_ce_edit_idx;
    if (idx < 0) {
        if (g_car_profile.sensor_count >= CAR_MAX_SENSORS) {
            ce_show_save_fail();
            return;
        }
        idx = g_car_profile.sensor_count++;
        memset(&g_car_profile.sensors[idx], 0, sizeof(CarSensor));
        g_car_profile.sensors[idx].slot = (uint8_t)idx;
    }
    CarSensor *s = &g_car_profile.sensors[idx];

    // Name
    if (s_ce_ta_name) {
        const char *n = lv_textarea_get_text(s_ce_ta_name);
        strncpy(s->name, n, CAR_NAME_LEN - 1);
        s->name[CAR_NAME_LEN - 1] = '\0';
    }
    // CAN ID (hex)
    if (s_ce_ta_canid) {
        s->can_id = (uint32_t)strtoul(lv_textarea_get_text(s_ce_ta_canid),
                                       nullptr, 16);
    }
    // Proto
    if (s_ce_dd_proto) {
        uint32_t sel = lv_dropdown_get_selected(s_ce_dd_proto);
        if (sel < sizeof(CAN_PROTO_VALUES)) s->proto = CAN_PROTO_VALUES[sel];
    }
    // Start, len, unsigned
    if (s_ce_ta_start)
        s->start = (uint8_t)atoi(lv_textarea_get_text(s_ce_ta_start));
    if (s_ce_ta_len)
        s->len = (uint8_t)atoi(lv_textarea_get_text(s_ce_ta_len));
    if (s_ce_sw_unsigned)
        s->is_unsigned = lv_obj_has_state(s_ce_sw_unsigned, LV_STATE_CHECKED);
    if (s_ce_ta_scale)
        s->scale = (float)atof(lv_textarea_get_text(s_ce_ta_scale));
    if (s_ce_ta_offset)
        s->offset = (float)atof(lv_textarea_get_text(s_ce_ta_offset));
    if (s_ce_ta_min)
        s->min_val = (float)atof(lv_textarea_get_text(s_ce_ta_min));
    if (s_ce_ta_max)
        s->max_val = (float)atof(lv_textarea_get_text(s_ce_ta_max));

    if (!ce_persist_active_profile()) {
        ce_show_save_fail();
        return;
    }
    ce_clear_state();
    open_can_channels_screen();
}

// --- Delete callback (in editor) -------------------------------------------
static void ce_delete_cb(lv_event_t * /*e*/) {
    int idx = s_ce_edit_idx;
    if (idx >= 0 && idx < g_car_profile.sensor_count) {
        // Compact the array
        for (int j = idx; j < g_car_profile.sensor_count - 1; j++) {
            g_car_profile.sensors[j] = g_car_profile.sensors[j + 1];
        }
        g_car_profile.sensor_count--;
        ce_persist_active_profile();
    }
    ce_clear_state();
    open_can_channels_screen();
}

static void ce_back_to_list_cb(lv_event_t * /*e*/) {
    ce_clear_state();
    open_can_channels_screen();
}

// --- Editor screen ----------------------------------------------------------
static void open_can_channel_edit(int slot_idx) {
    s_ce_edit_idx = slot_idx;
    bool is_new = (slot_idx < 0);
    CarSensor blank = {};
    const CarSensor *s = is_new ? &blank : &g_car_profile.sensors[slot_idx];

    lv_obj_t *action_btn = nullptr;
    lv_obj_t *scr = make_sub_screen(tr(TR_CAN_CH_EDIT), ce_back_to_list_cb,
                                    &action_btn, tr(TR_SAVE));
    if (action_btn)
        lv_obj_add_event_cb(action_btn, ce_save_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(content, 220, LV_STATE_DEFAULT);

    char buf[24];

    auto mk_field = [&](lv_obj_t *parent, const char *label, int x, int y,
                        int w, const char *initial, bool numeric) -> lv_obj_t* {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, label);
        brl_style_label(l, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        lv_obj_set_pos(l, x, y);

        lv_obj_t *ta = lv_textarea_create(parent);
        lv_obj_set_size(ta, w, 36);
        lv_obj_set_pos(ta, x, y + 22);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_text(ta, initial);
        lv_textarea_set_max_length(ta, 24);
        lv_obj_set_user_data(ta, numeric ? (void*)1 : nullptr);
        lv_obj_set_style_text_font(ta, &BRL_FONT_16, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(ta, 6, LV_STATE_DEFAULT);
        lv_obj_add_event_cb(ta, ce_ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
        return ta;
    };

    // Card 1: identity
    lv_obj_t *c1 = lv_obj_create(content);
    lv_obj_set_width(c1, LV_PCT(100));
    lv_obj_set_height(c1, 150);
    brl_style_card(c1);
    lv_obj_remove_flag(c1, LV_OBJ_FLAG_SCROLLABLE);

    s_ce_ta_name = mk_field(c1, tr(TR_ANALOG_NAME), 8, 8, 360,
                             s->name, false);
    snprintf(buf, sizeof(buf), "%lX", (unsigned long)s->can_id);
    s_ce_ta_canid = mk_field(c1, tr(TR_CAN_ID), 380, 8, 230, buf, true);

    // Proto dropdown (positioned next to name)
    lv_obj_t *p_lbl = lv_label_create(c1);
    lv_label_set_text(p_lbl, tr(TR_CAN_PROTO));
    brl_style_label(p_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(p_lbl, 8, 80);

    s_ce_dd_proto = lv_dropdown_create(c1);
    lv_dropdown_set_options(s_ce_dd_proto, CAN_PROTO_LABELS);
    lv_obj_set_size(s_ce_dd_proto, 360, 38);
    lv_obj_set_pos(s_ce_dd_proto, 8, 102);
    uint32_t sel = 0;
    for (uint32_t k = 0; k < sizeof(CAN_PROTO_VALUES); k++)
        if (CAN_PROTO_VALUES[k] == s->proto) sel = k;
    lv_dropdown_set_selected(s_ce_dd_proto, sel);

    // Card 2: bit/byte layout
    lv_obj_t *c2 = lv_obj_create(content);
    lv_obj_set_width(c2, LV_PCT(100));
    lv_obj_set_height(c2, 90);
    brl_style_card(c2);
    lv_obj_remove_flag(c2, LV_OBJ_FLAG_SCROLLABLE);

    snprintf(buf, sizeof(buf), "%u", (unsigned)s->start);
    s_ce_ta_start = mk_field(c2, tr(TR_CAN_START), 8, 8, 110, buf, true);
    snprintf(buf, sizeof(buf), "%u", (unsigned)(s->len ? s->len : 1));
    s_ce_ta_len = mk_field(c2, tr(TR_CAN_LEN),   140, 8, 110, buf, true);

    lv_obj_t *u_lbl = lv_label_create(c2);
    lv_label_set_text(u_lbl, tr(TR_CAN_UNSIGNED));
    brl_style_label(u_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(u_lbl, 280, 8);
    s_ce_sw_unsigned = lv_switch_create(c2);
    lv_obj_set_size(s_ce_sw_unsigned, 60, 32);
    lv_obj_set_pos(s_ce_sw_unsigned, 280, 30);
    if (s->is_unsigned) lv_obj_add_state(s_ce_sw_unsigned, LV_STATE_CHECKED);

    // Card 3: scaling
    lv_obj_t *c3 = lv_obj_create(content);
    lv_obj_set_width(c3, LV_PCT(100));
    lv_obj_set_height(c3, 90);
    brl_style_card(c3);
    lv_obj_remove_flag(c3, LV_OBJ_FLAG_SCROLLABLE);

    snprintf(buf, sizeof(buf), "%g", (double)(s->scale ? s->scale : 1.0f));
    s_ce_ta_scale = mk_field(c3, tr(TR_ANALOG_SCALE),  8,  8, 220, buf, true);
    snprintf(buf, sizeof(buf), "%g", (double)s->offset);
    s_ce_ta_offset = mk_field(c3, tr(TR_ANALOG_OFFSET), 250, 8, 220, buf, true);

    // Card 4: range + delete
    lv_obj_t *c4 = lv_obj_create(content);
    lv_obj_set_width(c4, LV_PCT(100));
    lv_obj_set_height(c4, 90);
    brl_style_card(c4);
    lv_obj_remove_flag(c4, LV_OBJ_FLAG_SCROLLABLE);

    snprintf(buf, sizeof(buf), "%g", (double)s->min_val);
    s_ce_ta_min = mk_field(c4, tr(TR_ANALOG_MIN),  8,  8, 180, buf, true);
    snprintf(buf, sizeof(buf), "%g", (double)s->max_val);
    s_ce_ta_max = mk_field(c4, tr(TR_ANALOG_MAX), 200, 8, 180, buf, true);

    if (!is_new) {
        lv_obj_t *del_btn = lv_button_create(c4);
        lv_obj_set_size(del_btn, 160, 44);
        lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_bg_color(del_btn, BRL_CLR_DANGER, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(del_btn, 6, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(del_btn, 0, LV_STATE_DEFAULT);
        lv_obj_t *del_lbl = lv_label_create(del_btn);
        lv_label_set_text(del_lbl, LV_SYMBOL_TRASH "  Löschen");
        brl_style_label(del_lbl, &BRL_FONT_16, BRL_CLR_TEXT);
        lv_obj_center(del_lbl);
        lv_obj_add_event_cb(del_btn, ce_delete_cb, LV_EVENT_CLICKED, nullptr);
    }

    // Shared keyboard
    s_ce_kb = lv_keyboard_create(scr);
    lv_obj_set_size(s_ce_kb, BRL_SCREEN_W, 200);
    lv_obj_align(s_ce_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_ce_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ce_kb, ce_kb_done_cb, LV_EVENT_READY,  nullptr);
    lv_obj_add_event_cb(s_ce_kb, ce_kb_done_cb, LV_EVENT_CANCEL, nullptr);

    sub_screen_load(scr);
}

// --- List screen ------------------------------------------------------------
static void open_can_channels_screen() {
    ce_clear_state();
    s_ce_edit_idx = -1;

    lv_obj_t *scr = make_sub_screen(tr(TR_CAN_CH_TITLE),
                                    [](lv_event_t * /*e*/){ open_settings_screen(); });
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    if (!g_car_profile.loaded) {
        lv_obj_t *ph = lv_label_create(content);
        lv_label_set_text(ph, tr(TR_CAN_NO_PROFILE));
        brl_style_label(ph, &BRL_FONT_16, BRL_CLR_DANGER);
        sub_screen_load(scr);
        return;
    }

    // Header info: which profile
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s %s — %d Sensoren",
             g_car_profile.make, g_car_profile.engine,
             g_car_profile.sensor_count);
    lv_obj_t *info = lv_label_create(content);
    lv_label_set_text(info, hdr);
    brl_style_label(info, &BRL_FONT_16, BRL_CLR_ACCENT);

    // Add-button
    lv_obj_t *add_btn = lv_button_create(content);
    lv_obj_set_width(add_btn, LV_PCT(100));
    lv_obj_set_height(add_btn, 50);
    lv_obj_set_style_bg_color(add_btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(add_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(add_btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, tr(TR_CAN_CH_ADD));
    brl_style_label(add_lbl, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_center(add_lbl);
    lv_obj_add_event_cb(add_btn, [](lv_event_t * /*e*/){
        open_can_channel_edit(-1);
    }, LV_EVENT_CLICKED, nullptr);

    // Sensor rows
    for (int i = 0; i < g_car_profile.sensor_count; i++) {
        const CarSensor *s = &g_car_profile.sensors[i];
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 56);
        brl_style_card(row);
        lv_obj_set_style_radius(row, 0, LV_STATE_DEFAULT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t *e){
            int idx = (int)(intptr_t)lv_obj_get_user_data(
                (lv_obj_t*)lv_event_get_current_target(e));
            open_can_channel_edit(idx);
        }, LV_EVENT_CLICKED, nullptr);

        char rowtxt[96];
        const char *proto_name =
            (s->proto == 7) ? "OBD2" :
            (s->proto == 1) ? "UDS"  : "PT-CAN";
        snprintf(rowtxt, sizeof(rowtxt), "%-15s  %s ID:%lX  start:%u len:%u",
                 s->name, proto_name, (unsigned long)s->can_id,
                 (unsigned)s->start, (unsigned)s->len);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, rowtxt);
        brl_style_label(l, &BRL_FONT_16, BRL_CLR_TEXT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *arr = lv_label_create(row);
        lv_label_set_text(arr, LV_SYMBOL_RIGHT);
        brl_style_label(arr, &BRL_FONT_20, BRL_CLR_TEXT_DIM);
        lv_obj_align(arr, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    sub_screen_load(scr);
}

// ============================================================================
// CAR PROFILE MANAGER SCREEN
// ============================================================================
static void open_car_profiles_screen();

// Merged profile entry for display
struct ProfileItem {
    char filename[CAR_NAME_LEN];
    char make[CAR_NAME_LEN];
    char display[CAR_NAME_LEN];
    bool local;
    bool active;
};

static ProfileItem s_profiles[50];
static int         s_n_profiles = 0;
static char        s_filter_make[CAR_NAME_LEN] = {};  // empty = show all

static void open_car_profiles_screen() {
    lv_obj_t *scr = make_sub_screen(tr(TR_CAR_PROFILES), cb_back_to_menu);
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    // Get active profile
    char active[CAR_NAME_LEN] = {};
    car_profile_get_active(active, sizeof(active));

    // Get local profiles on SD
    static char local_files[20][CAR_NAME_LEN];
    int n_local = car_profile_list(local_files, 20);

    // Get server profiles (needs WiFi)
    static CarProfileEntry server_entries[40];
    int n_server = 0;

    bool wifi_on = (g_state.wifi_mode == BRL_WIFI_AP || g_state.wifi_mode == BRL_WIFI_STA);

    if (wifi_on) {
        lv_obj_t *loading = lv_label_create(content);
        lv_label_set_text(loading, tr(TR_CAR_LOADING));
        brl_style_label(loading, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_refr_now(nullptr);

        n_server = car_profile_fetch_list(server_entries, 40);
        lv_obj_delete(loading);
    }

    // Build merged profile list
    s_n_profiles = 0;

    auto is_local = [&](const char *name) -> bool {
        for (int i = 0; i < n_local; i++)
            if (strcmp(local_files[i], name) == 0) return true;
        return false;
    };

    // Add server profiles first (they have make/display metadata)
    for (int i = 0; i < n_server && s_n_profiles < 50; i++) {
        auto &p = s_profiles[s_n_profiles];
        strncpy(p.filename, server_entries[i].filename, CAR_NAME_LEN - 1);
        strncpy(p.make, server_entries[i].make, CAR_NAME_LEN - 1);
        strncpy(p.display, server_entries[i].display, CAR_NAME_LEN - 1);
        p.local = is_local(p.filename);
        p.active = (strlen(active) > 0 && strcmp(active, p.filename) == 0);
        s_n_profiles++;
    }

    // Add local-only profiles (not on server)
    for (int i = 0; i < n_local && s_n_profiles < 50; i++) {
        bool on_server = false;
        for (int j = 0; j < n_server; j++) {
            if (strcmp(local_files[i], server_entries[j].filename) == 0) {
                on_server = true; break;
            }
        }
        if (!on_server) {
            auto &p = s_profiles[s_n_profiles];
            strncpy(p.filename, local_files[i], CAR_NAME_LEN - 1);
            p.make[0] = '\0';
            // Display: strip .brl extension
            strncpy(p.display, local_files[i], CAR_NAME_LEN - 1);
            char *dot = strrchr(p.display, '.');
            if (dot) *dot = '\0';
            p.local = true;
            p.active = (strlen(active) > 0 && strcmp(active, p.filename) == 0);
            s_n_profiles++;
        }
    }

    // Collect unique makes for filter bar
    char makes[10][CAR_NAME_LEN] = {};
    int n_makes = 0;
    for (int i = 0; i < s_n_profiles; i++) {
        if (s_profiles[i].make[0] == '\0') continue;
        bool found = false;
        for (int j = 0; j < n_makes; j++) {
            if (strcmp(makes[j], s_profiles[i].make) == 0) { found = true; break; }
        }
        if (!found && n_makes < 10) {
            strncpy(makes[n_makes++], s_profiles[i].make, CAR_NAME_LEN - 1);
        }
    }

    // Filter bar (only if we have more than one make)
    if (n_makes > 1) {
        lv_obj_t *filter_row = lv_obj_create(content);
        lv_obj_set_width(filter_row, LV_PCT(100));
        lv_obj_set_height(filter_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(filter_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(filter_row, 4, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_column(filter_row, 6, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(filter_row, LV_OPA_TRANSP, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(filter_row, 0, LV_STATE_DEFAULT);
        lv_obj_remove_flag(filter_row, LV_OBJ_FLAG_SCROLLABLE);

        // "All" button
        bool all_selected = (s_filter_make[0] == '\0');
        lv_obj_t *btn_all = lv_button_create(filter_row);
        lv_obj_set_height(btn_all, 32);
        lv_obj_set_width(btn_all, LV_SIZE_CONTENT);
        brl_style_btn(btn_all, all_selected ? BRL_CLR_ACCENT : lv_color_hex(0x333333));
        lv_obj_t *lbl_all = lv_label_create(btn_all);
        lv_label_set_text(lbl_all, tr(TR_CAR_ALL_MAKES));
        brl_style_label(lbl_all, &BRL_FONT_14, BRL_CLR_TEXT);
        lv_obj_center(lbl_all);
        lv_obj_add_event_cb(btn_all, [](lv_event_t *e) {
            (void)e;
            s_filter_make[0] = '\0';
            open_car_profiles_screen();
        }, LV_EVENT_CLICKED, nullptr);

        // Per-make buttons
        for (int m = 0; m < n_makes; m++) {
            bool selected = (strcmp(s_filter_make, makes[m]) == 0);
            lv_obj_t *btn = lv_button_create(filter_row);
            lv_obj_set_height(btn, 32);
            lv_obj_set_width(btn, LV_SIZE_CONTENT);
            brl_style_btn(btn, selected ? BRL_CLR_ACCENT : lv_color_hex(0x333333));
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, makes[m]);
            brl_style_label(lbl, &BRL_FONT_14, BRL_CLR_TEXT);
            lv_obj_center(lbl);
            // Store make index in user_data
            lv_obj_set_user_data(btn, (void*)(intptr_t)m);
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
                // Lookup make name from s_profiles
                char target[CAR_NAME_LEN] = {};
                int seen = 0;
                for (int i = 0; i < s_n_profiles; i++) {
                    if (s_profiles[i].make[0] == '\0') continue;
                    bool dup = false;
                    for (int j = 0; j < i; j++) {
                        if (strcmp(s_profiles[j].make, s_profiles[i].make) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        if (seen == idx) {
                            strncpy(target, s_profiles[i].make, CAR_NAME_LEN - 1);
                            break;
                        }
                        seen++;
                    }
                }
                strncpy(s_filter_make, target, CAR_NAME_LEN - 1);
                open_car_profiles_screen();
            }, LV_EVENT_CLICKED, nullptr);
        }
    }

    if (!wifi_on && s_n_profiles == 0) {
        lv_obj_t *hint = lv_label_create(content);
        lv_label_set_text(hint, tr(TR_CAR_NO_WIFI));
        brl_style_label(hint, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
    }

    for (int i = 0; i < s_n_profiles; i++) {
        auto &p = s_profiles[i];

        // Apply make filter
        if (s_filter_make[0] != '\0' && strcmp(p.make, s_filter_make) != 0)
            continue;

        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 60);
        brl_style_card(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Profile display name (make + display)
        char label_text[64];
        if (p.make[0] != '\0') {
            snprintf(label_text, sizeof(label_text), "%s  %s", p.make, p.display);
        } else {
            strncpy(label_text, p.display, sizeof(label_text) - 1);
            label_text[sizeof(label_text) - 1] = '\0';
        }

        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, label_text);
        brl_style_label(name_lbl, &BRL_FONT_20, p.active ? BRL_CLR_ACCENT : BRL_CLR_TEXT);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, -8);

        // Status label
        lv_obj_t *status_lbl = lv_label_create(row);
        if (p.active) {
            lv_label_set_text_fmt(status_lbl, LV_SYMBOL_OK "  %s", tr(TR_CAR_ACTIVE));
            brl_style_label(status_lbl, &BRL_FONT_14, lv_color_hex(0x00CC66));
        } else if (p.local) {
            lv_label_set_text(status_lbl, tr(TR_CAR_ON_DEVICE));
            brl_style_label(status_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        } else {
            lv_label_set_text(status_lbl, tr(TR_CAR_DOWNLOAD));
            brl_style_label(status_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
        }
        lv_obj_align(status_lbl, LV_ALIGN_LEFT_MID, 0, 10);

        // Action buttons (right side)
        if (p.local && !p.active) {
            // Activate button
            lv_obj_t *btn_act = lv_button_create(row);
            lv_obj_set_size(btn_act, 120, 36);
            lv_obj_align(btn_act, LV_ALIGN_RIGHT_MID, -70, 0);
            brl_style_btn(btn_act, BRL_CLR_ACCENT);
            lv_obj_t *al = lv_label_create(btn_act);
            lv_label_set_text(al, tr(TR_CAR_ACTIVATE));
            brl_style_label(al, &BRL_FONT_14, BRL_CLR_TEXT);
            lv_obj_center(al);
            lv_obj_set_user_data(btn_act, (void*)(intptr_t)i);
            lv_obj_add_event_cb(btn_act, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
                car_profile_set_active(s_profiles[idx].filename);
                car_profile_load(s_profiles[idx].filename);
                open_car_profiles_screen();
            }, LV_EVENT_CLICKED, nullptr);

            // Delete button
            lv_obj_t *btn_del = lv_button_create(row);
            lv_obj_set_size(btn_del, 48, 36);
            lv_obj_align(btn_del, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(btn_del, lv_color_hex(0x5A1A1A), LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(btn_del, BRL_CLR_DANGER, LV_STATE_PRESSED);
            lv_obj_set_style_border_width(btn_del, 0, LV_STATE_DEFAULT);
            lv_obj_set_style_radius(btn_del, 6, LV_STATE_DEFAULT);
            lv_obj_set_style_shadow_width(btn_del, 0, LV_STATE_DEFAULT);
            lv_obj_t *dl = lv_label_create(btn_del);
            lv_label_set_text(dl, LV_SYMBOL_TRASH);
            brl_style_label(dl, &BRL_FONT_16, lv_color_hex(0xFF6666));
            lv_obj_center(dl);
            lv_obj_set_user_data(btn_del, (void*)(intptr_t)i);
            lv_obj_add_event_cb(btn_del, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
                car_profile_delete(s_profiles[idx].filename);
                open_car_profiles_screen();
            }, LV_EVENT_CLICKED, nullptr);
        } else if (!p.local) {
            // Download button
            lv_obj_t *btn_dl = lv_button_create(row);
            lv_obj_set_size(btn_dl, 140, 36);
            lv_obj_align(btn_dl, LV_ALIGN_RIGHT_MID, 0, 0);
            brl_style_btn(btn_dl, BRL_CLR_ACCENT);
            lv_obj_t *dll = lv_label_create(btn_dl);
            lv_label_set_text_fmt(dll, LV_SYMBOL_SAVE " %s", tr(TR_CAR_DOWNLOAD));
            brl_style_label(dll, &BRL_FONT_14, BRL_CLR_TEXT);
            lv_obj_center(dll);
            lv_obj_set_user_data(btn_dl, (void*)(intptr_t)i);
            lv_obj_add_event_cb(btn_dl, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_current_target(e));
                bool ok = car_profile_download(s_profiles[idx].filename);
                (void)ok;
                open_car_profiles_screen();
            }, LV_EVENT_CLICKED, nullptr);
        }
    }

    sub_screen_load(scr);
}

static void open_settings_screen() {
    lv_obj_t *scr = make_sub_screen(tr(TR_SETTINGS_TITLE), cb_back_to_menu);
    lv_obj_t *content = build_content_area(scr, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, LV_STATE_DEFAULT);

    const int RH = 56, RH2 = 68;

    // Vehicle connection mode (OBD BLE / CAN direct)
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_DRIVE,
                                        tr(TR_VEH_CONN_TITLE), tr(TR_VEH_CONN_SUB));

        // Status label
        set_obd_status_lbl = lv_label_create(r);
        if (g_state.veh_conn_mode == VEH_CONN_CAN_DIRECT) {
            if (can_bus_active()) {
                lv_label_set_text(set_obd_status_lbl, tr(TR_CONNECTED));
                brl_style_label(set_obd_status_lbl, &BRL_FONT_16, lv_color_hex(0x00CC66));
            } else if (!g_car_profile.loaded) {
                lv_label_set_text(set_obd_status_lbl, tr(TR_VEH_CAN_NO_PROFILE));
                brl_style_label(set_obd_status_lbl, &BRL_FONT_16, BRL_CLR_DANGER);
            } else {
                lv_label_set_text(set_obd_status_lbl, tr(TR_NOT_CONNECTED));
                brl_style_label(set_obd_status_lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
            }
        } else {
            lv_label_set_text(set_obd_status_lbl, tr(TR_NOT_CONNECTED));
            brl_style_label(set_obd_status_lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        }
        lv_obj_set_width(set_obd_status_lbl, 140);
        lv_obj_align(set_obd_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        // OBD BLE button
        bool is_obd = (g_state.veh_conn_mode == VEH_CONN_OBD_BLE);
        lv_obj_t *btn_obd = make_setting_btn(r, tr(TR_VEH_OBD_BLE),
            is_obd ? BRL_CLR_ACCENT : lv_color_hex(0x333333), LV_ALIGN_RIGHT_MID, -160);
        lv_obj_add_event_cb(btn_obd, [](lv_event_t* /*e*/){
            if (g_state.veh_conn_mode == VEH_CONN_CAN_DIRECT) {
                can_bus_stop();
            }
            g_state.veh_conn_mode = VEH_CONN_OBD_BLE;
            g_dash_cfg.veh_conn_mode = VEH_CONN_OBD_BLE;
            dash_config_save();
            open_settings_screen();
        }, LV_EVENT_CLICKED, nullptr);

        // CAN Direct button
        bool is_can = (g_state.veh_conn_mode == VEH_CONN_CAN_DIRECT);
        lv_obj_t *btn_can = make_setting_btn(r, tr(TR_VEH_CAN_DIRECT),
            is_can ? BRL_CLR_ACCENT : lv_color_hex(0x333333), LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(btn_can, [](lv_event_t* /*e*/){
            if (g_state.veh_conn_mode == VEH_CONN_OBD_BLE) {
                obd_bt_disconnect();
            }
            g_state.veh_conn_mode = VEH_CONN_CAN_DIRECT;
            g_dash_cfg.veh_conn_mode = VEH_CONN_CAN_DIRECT;
            dash_config_save();
            // Start CAN if profile is loaded
            if (g_car_profile.loaded && !can_bus_active()) {
                can_bus_init();
            }
            open_settings_screen();
        }, LV_EVENT_CLICKED, nullptr);
    }
    // GPS Info
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_GPS,
                                        tr(TR_GPS_INFO), tr(TR_GPS_INFO_SUB));
        lv_obj_t *gps_status = lv_label_create(r);
        if (g_state.gps.valid) {
            char sat_buf[16];
            snprintf(sat_buf, sizeof(sat_buf), "%d Sats", g_state.gps.satellites);
            lv_label_set_text(gps_status, sat_buf);
            brl_style_label(gps_status, &BRL_FONT_16, lv_color_hex(0x00CC66));
        } else {
            lv_label_set_text(gps_status, tr(TR_GPS_NO_FIX));
            brl_style_label(gps_status, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        }
        lv_obj_set_width(gps_status, 140);
        lv_obj_align(gps_status, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *bgps = make_setting_btn(r, "Info", BRL_CLR_ACCENT, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(bgps, [](lv_event_t* /*e*/){ open_gps_info_screen(); },
                            LV_EVENT_CLICKED, nullptr);
    }
    // WiFi AP — live status, on/off toggle, configure dialog
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_WIFI,
                                        tr(TR_WIFI_AP_TITLE), tr(TR_WIFI_AP_SUB));

        // Status: "AP aktiv\n<SSID> @ 192.168.4.1" if on, else "Aus"
        set_wifi_ap_status_lbl = lv_label_create(r);
        bool ap_on = (g_state.wifi_mode == BRL_WIFI_AP);
        char st_buf[96];
        if (ap_on) {
            snprintf(st_buf, sizeof(st_buf), "%s\n%s\n%s",
                     tr(TR_WIFI_AP_ON), wifi_ap_ssid(), wifi_ap_ip());
        } else {
            snprintf(st_buf, sizeof(st_buf), "%s", tr(TR_WIFI_AP_OFF));
        }
        lv_label_set_text(set_wifi_ap_status_lbl, st_buf);
        brl_style_label(set_wifi_ap_status_lbl, &BRL_FONT_14,
                        ap_on ? BRL_CLR_ACCENT : BRL_CLR_TEXT_DIM);
        lv_obj_set_width(set_wifi_ap_status_lbl, 220);
        lv_obj_align(set_wifi_ap_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        // On/Off toggle (primary action — otherwise user cannot enable AP)
        const char *toggle_lbl = ap_on ? tr(TR_DISCONNECT_BTN) : tr(TR_WIFI_AP_ON);
        lv_obj_t *btog = make_setting_btn(r, toggle_lbl,
            ap_on ? BRL_CLR_SURFACE2 : BRL_CLR_ACCENT,
            LV_ALIGN_RIGHT_MID, -150);
        lv_obj_add_event_cb(btog, [](lv_event_t* /*e*/){
            if (g_state.wifi_mode == BRL_WIFI_AP) wifi_set_mode(BRL_WIFI_OFF);
            else                                   wifi_set_mode(BRL_WIFI_AP);
            open_settings_screen();
        }, LV_EVENT_CLICKED, nullptr);

        // Configure dialog (SSID/password)
        char cfg_lbl[48];
        snprintf(cfg_lbl, sizeof(cfg_lbl), LV_SYMBOL_SETTINGS " %s", tr(TR_CONFIGURE_BTN));
        lv_obj_t *bcfg = make_setting_btn(r, cfg_lbl, BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(bcfg, [](lv_event_t* /*e*/){ open_wifi_ap_dialog(); },
                            LV_EVENT_CLICKED, nullptr);
    }
    // WiFi STA
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_WIFI,
                                        tr(TR_WIFI_STA_TITLE), tr(TR_WIFI_STA_SUB));
        set_wifi_sta_status_lbl = lv_label_create(r);
        lv_label_set_text(set_wifi_sta_status_lbl, tr(TR_NOT_CONNECTED));
        brl_style_label(set_wifi_sta_status_lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        lv_obj_set_width(set_wifi_sta_status_lbl, 120);
        lv_obj_align(set_wifi_sta_status_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        char cfg_lbl[48];
        snprintf(cfg_lbl, sizeof(cfg_lbl), LV_SYMBOL_WIFI " %s", tr(TR_WIFI_SCAN));
        lv_obj_t *bcfg = make_setting_btn(r, cfg_lbl, BRL_CLR_ACCENT, LV_ALIGN_RIGHT_MID, -150);
        lv_obj_add_event_cb(bcfg, [](lv_event_t* /*e*/){ open_wifi_sta_dialog(); },
                            LV_EVENT_CLICKED, nullptr);
        char dis_lbl[48];
        snprintf(dis_lbl, sizeof(dis_lbl), LV_SYMBOL_CLOSE " %s", tr(TR_DISCONNECT_BTN));
        lv_obj_t *bdis = make_setting_btn(r, dis_lbl, BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(bdis, [](lv_event_t* /*e*/){ wifi_set_mode(BRL_WIFI_OFF); },
                            LV_EVENT_CLICKED, nullptr);
    }
    // Car Profiles
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_DRIVE,
                                        tr(TR_CAR_PROFILES), tr(TR_CAR_PROFILES_SUB));
        // Show active profile name if loaded
        lv_obj_t *prof_lbl = lv_label_create(r);
        if (g_car_profile.loaded) {
            char prof_txt[48];
            snprintf(prof_txt, sizeof(prof_txt), "%s %s", g_car_profile.make, g_car_profile.engine);
            lv_label_set_text(prof_lbl, prof_txt);
            brl_style_label(prof_lbl, &BRL_FONT_16, lv_color_hex(0x00CC66));
        } else {
            lv_label_set_text(prof_lbl, "---");
            brl_style_label(prof_lbl, &BRL_FONT_16, BRL_CLR_TEXT_DIM);
        }
        lv_obj_set_width(prof_lbl, 140);
        lv_obj_align(prof_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *bopen = make_setting_btn(r, tr(TR_CONFIGURE_BTN), BRL_CLR_ACCENT, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(bopen, [](lv_event_t* /*e*/){ open_car_profiles_screen(); },
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
    // Analog inputs (AN1-AN4 calibration)
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_CHARGE,
                                        tr(TR_ANALOG_TITLE), tr(TR_ANALOG_SUB));
        lv_obj_t *btn = make_setting_btn(r, tr(TR_CONFIGURE_BTN),
                                         BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(btn, [](lv_event_t * /*e*/) {
            open_analog_screen();
        }, LV_EVENT_CLICKED, nullptr);
    }
    // Custom CAN channel editor (RaceCapture-style)
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_LIST,
                                        tr(TR_CAN_CH_TITLE), tr(TR_CAN_CH_SUB));
        lv_obj_t *btn = make_setting_btn(r, tr(TR_CONFIGURE_BTN),
                                         BRL_CLR_SURFACE2, LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(btn, [](lv_event_t * /*e*/) {
            open_can_channels_screen();
        }, LV_EVENT_CLICKED, nullptr);
    }
    // Engine data (Zone 3) visibility
    {
        lv_obj_t *r = make_setting_row(content, 0, RH2, LV_SYMBOL_EYE_OPEN,
                                        tr(TR_SHOW_OBD), tr(TR_SHOW_OBD_SUB));
        lv_obj_t *btn = make_setting_btn(r,
            g_dash_cfg.show_obd ? tr(TR_WIFI_AP_ON) : tr(TR_WIFI_AP_OFF),
            g_dash_cfg.show_obd ? BRL_CLR_ACCENT : BRL_CLR_SURFACE2,
            LV_ALIGN_RIGHT_MID);
        lv_obj_add_event_cb(btn, [](lv_event_t *e){
            g_dash_cfg.show_obd = g_dash_cfg.show_obd ? 0 : 1;
            dash_config_save();
            lv_obj_t *b = (lv_obj_t*)lv_event_get_current_target(e);
            lv_label_set_text(lv_obj_get_child(b, 0),
                g_dash_cfg.show_obd ? tr(TR_WIFI_AP_ON) : tr(TR_WIFI_AP_OFF));
            lv_obj_set_style_bg_color(b,
                g_dash_cfg.show_obd ? BRL_CLR_ACCENT : BRL_CLR_SURFACE2,
                LV_STATE_DEFAULT);
            // Rebuild timing screen so Zone 3 appears/disappears on next visit.
            timing_screen_rebuild();
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
            uint64_t total_b = 0ULL /* TODO: implement sd_total_bytes() */;
            uint64_t used_b  = 0ULL /* TODO: implement sd_used_bytes() */;
            float total_gb   = (float)total_b / 1073741824.0f;
            float used_gb    = (float)used_b  / 1073741824.0f;
            int pct_used     = (total_b > 0)
                               ? (int)((float)used_b / (float)total_b * 100.0f) : 0;
            char sbuf[64];
            snprintf(sbuf, sizeof(sbuf), "%.1f GB %s / %.1f GB",
                     used_gb, tr(TR_STORAGE_USED), total_gb);
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

    // GPS label: show arrow + Hz when fix, just arrow (gray) when no fix
    if (sb.gps) {
        if (g_state.gps.valid) {
            char buf[24];
            uint8_t hz = gps_get_update_rate();
            snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %dHz %dSat",
                     (int)hz, (int)g_state.gps.satellites);
            lv_label_set_text(sb.gps, buf);
            lv_obj_set_style_text_color(sb.gps, lv_color_hex(0x00CC66), 0);
        } else {
            lv_label_set_text(sb.gps, LV_SYMBOL_GPS);
            lv_obj_set_style_text_color(sb.gps, lv_color_hex(0xAAAAAA), 0);
        }
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
    // Recording indicator — driven by cam_link status (external cam module).
    if (sb.rec) {
        CamLinkInfo ci = cam_link_get_info();
        if (ci.link_up && ci.status.rec_active) {
            lv_label_set_text(sb.rec, LV_SYMBOL_VIDEO " REC");
            lv_obj_set_style_text_color(sb.rec, lv_color_hex(0xFF3030), 0);
        } else if (ci.link_up) {
            lv_label_set_text(sb.rec, LV_SYMBOL_VIDEO);
            lv_obj_set_style_text_color(sb.rec, lv_color_hex(0x666666), 0);
        } else {
            lv_label_set_text(sb.rec, "");
        }
    }
    // Vehicle connection label (auto icon + OBD/CAN mode)
    if (sb.obd) {
        if (g_state.veh_conn_mode == VEH_CONN_CAN_DIRECT) {
            bool active = can_bus_active();
            lv_label_set_text(sb.obd, active ? LV_SYMBOL_DRIVE " CAN" : LV_SYMBOL_DRIVE " CAN");
            lv_obj_set_style_text_color(sb.obd,
                active ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
        } else {
            bool conn = obd_is_connected();
            lv_label_set_text(sb.obd, conn ? LV_SYMBOL_DRIVE " OBD" : LV_SYMBOL_DRIVE " OBD");
            lv_obj_set_style_text_color(sb.obd,
                conn ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
        }
    }
}

void timer_live_update(lv_timer_t * /*t*/) {
    // PERF: only touch widgets that live on the currently-visible screen.
    // Updating invisible labels still costs CPU + causes LVGL to flag areas
    // dirty on their (inactive) parent screen, which showed up as scroll
    // stutter on the 1024×600 UI. Each status-bar/timing/settings block is
    // gated by a cheap screen-equality check.
    lv_obj_t *active_scr = lv_screen_active();
    auto on_active = [&](lv_obj_t *any_obj) -> bool {
        return any_obj && lv_obj_get_screen(any_obj) == active_scr;
    };

    // Update status bars — only the one on the active screen
    if (on_active(sb_menu.gps))   update_sb(sb_menu);
    if (on_active(sb_sub.gps))    update_sb(sb_sub);
    if (on_active(sb_timing.gps)) update_sb(sb_timing);

    // Timing-screen-only widgets: skip entirely when timing screen not shown
    const bool timing_active = on_active(sb_timing.gps);

    // PERF: keep LVGL render pipeline hot in L2 cache.
    // On menu/sub screens the only per-tick work is a few status-bar label
    // updates — if none of them actually change, LVGL skips the draw cycle
    // entirely. During long user-idle periods WiFi/SD/USB tasks pollute the
    // 256 KB L2 cache and LVGL's draw+scroll code gets evicted; first
    // scroll gesture then stutters because all that code must page in from
    // flash. Timing screen doesn't have this problem because its delta-bar
    // + live labels redraw every tick.
    //
    // Solution: on non-timing screens, force a full-screen invalidate
    // every ~400 ms. LVGL redraws identical pixels (no visible change),
    // but the render/draw code stays resident in L2 — first user scroll
    // is then as smooth as post-timing.
    if (!timing_active) {
        // Keep LVGL's render code path hot in the 256 KB L2 cache during
        // long idle periods on menu/sub screens. Without this, WiFi/SD/USB
        // tasks evict the draw+scroll code and the first scroll gesture
        // stutters while everything pages in from flash.
        //
        // Earlier revision invalidated the full 1024×600 screen every 100 ms
        // — that re-renders ~600k pixels 10×/s with DRAW_UNIT=1 (HARD RULE)
        // which is expensive enough to compete with touch gestures on Core 1.
        //
        // Fix: invalidate a tiny 32×32 area in the top-left corner every
        // tick. LVGL still walks its dirty-rect pipeline (cache stays warm)
        // but actual blit work is negligible (~1k pixels/tick).
        if (active_scr) {
            lv_area_t a = { 0, 0, 31, 31 };
            lv_obj_invalidate_area(active_scr, &a);
        }
    }

    // ---- Timing screen slot updates ----

    // Helper: only flag the label dirty when the text actually changed.
    // Large Zone-1 fonts (96/160 pt) are expensive to re-rasterise and
    // unconditional set-text at 10 Hz dragged the timing screen down after
    // the .tbrl bundle added PSRAM pressure.
    auto lbl_set = [](lv_obj_t *lbl, const char *txt) {
        if (!lbl || !txt) return;
        const char *cur = lv_label_get_text(lbl);
        if (cur && strcmp(cur, txt) == 0) return;
        lv_label_set_text(lbl, txt);
    };

    // Helper: format a single FieldId → text, write into lv label (null-safe)
    auto update_slot = [&](lv_obj_t *lbl, uint8_t fid) {
        if (!lbl) return;
        char buf[24];
        const LiveTiming  &lt   = g_state.timing;
        const LapSession  &sess = g_state.session;
        const ObdData     &obd  = g_state.obd;

        switch (fid) {
            case FIELD_SPEED: {
                float spd = g_state.units == 0
                            ? g_state.gps.speed_kmh
                            : g_state.gps.speed_kmh * 0.621371f;
                snprintf(buf, sizeof(buf), "%.0f", spd);
                break;
            }
            case FIELD_LAPTIME: {
                // While a run is in progress (in_lap), show the live elapsed
                // time. After a lap finishes (in_lap cleared, but timing_active
                // stays true to keep the value on screen), freeze on the last
                // lap's total_ms so the driver can read it after crossing the
                // finish line.
                uint32_t ms = 0;
                if (lt.in_lap)
                    ms = millis() - lt.lap_start_ms;
                else if (sess.lap_count > 0)
                    ms = sess.laps[sess.lap_count - 1].total_ms;
                snprintf(buf, sizeof(buf), "%u:%05.2f",
                         (unsigned)(ms / 60000), fmod(ms / 1000.0f, 60.0f));
                break;
            }
            case FIELD_BESTLAP: {
                if (sess.lap_count > 0) {
                    uint32_t ms = sess.laps[sess.best_lap_idx].total_ms;
                    snprintf(buf, sizeof(buf), "%u:%05.2f",
                             (unsigned)(ms / 60000), fmod(ms / 1000.0f, 60.0f));
                } else strncpy(buf, "---", sizeof(buf));
                break;
            }
            case FIELD_DELTA_NUM: {
                int32_t d = lt.live_delta_ms;
                snprintf(buf, sizeof(buf), "%+.2f s", d / 1000.0f);
                lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
                lbl_set(lbl, buf);
                return;
            }
            case FIELD_LAP_NR:
                snprintf(buf, sizeof(buf), "%d", (int)lt.lap_number);
                break;
            case FIELD_SECTOR1:
            case FIELD_SECTOR2:
            case FIELD_SECTOR3: {
                int si = fid - FIELD_SECTOR1;  // 0-based sector index
                uint32_t now_ms = millis();
                auto sec_fmt = [](char *b, size_t len, uint8_t n, uint32_t ms) {
                    char t[16];
                    fmt_sector_time(t, sizeof(t), ms);
                    snprintf(b, len, "S%u %s", (unsigned)n, t);
                };

                // --- F1-style sector coloring --------------------------------
                // Purple : new all-time best (beats the scan-loaded record)
                // Green  : new personal best this session for this sector
                //          (or the lap that currently HOLDS the session best)
                // Red    : slower than the session best
                // Blue   : sector is currently running (set below as ACCENT)
                //
                // best_in_session(si, excl): min sector_ms[si] over
                //   completed laps whose index != excl. excl=-1 = no exclusion.
                // Excluding the lap being displayed lets us answer "was this
                // faster than everything before it" rather than tautologically
                // matching itself.
                auto best_in_session = [&](int excl) -> uint32_t {
                    uint32_t best = 0;
                    for (uint8_t L = 0; L < sess.lap_count; L++) {
                        if ((int)L == excl) continue;
                        if (!sess.laps[L].valid) continue;
                        uint32_t t = sess.laps[L].sector_ms[si];
                        if (t > 0 && (best == 0 || t < best)) best = t;
                    }
                    return best;  // 0 = no data
                };
                uint32_t alltime_best = lap_timer_alltime_sector_best((uint8_t)si);

                auto color_for = [&](uint32_t T, uint32_t session_best_prev) -> lv_color_t {
                    if (T == 0) return BRL_CLR_TEXT;
                    if (alltime_best > 0 && T < alltime_best)
                        return BRL_CLR_PURPLE;
                    if (session_best_prev == 0 || T < session_best_prev)
                        return BRL_CLR_OK;      // green
                    if (T == session_best_prev)
                        return BRL_CLR_OK;      // matching session best → green
                    return BRL_CLR_DANGER;      // red
                };

                lv_color_t clr = BRL_CLR_TEXT;

                if (lt.in_lap) {
                    const uint32_t *s_ms = sess.laps[sess.lap_count].sector_ms;
                    uint8_t cs      = lt.current_sector;
                    uint32_t run_ms = now_ms - lt.sector_start_ms;
                    if ((int)cs > si) {
                        sec_fmt(buf, sizeof(buf), si + 1, s_ms[si]);
                        // This sector just completed on the CURRENT lap.
                        // Session best "before this sector" = best over
                        // completed laps (current lap not yet in sess.laps).
                        clr = color_for(s_ms[si], best_in_session(-1));
                    } else if ((int)cs == si) {
                        fmt_sector_time(buf, sizeof(buf), run_ms);
                        clr = BRL_CLR_ACCENT;   // running = blue
                    } else {
                        strncpy(buf, "---", sizeof(buf));
                        clr = BRL_CLR_TEXT_DIM;
                    }
                } else if (sess.lap_count > 0) {
                    // Between laps — show last completed lap's sector.
                    // Exclude it from the "session best prev" so the very
                    // lap that SET the session best still colors green.
                    int last_idx = sess.lap_count - 1;
                    uint32_t s = sess.laps[last_idx].sector_ms[si];
                    if (s > 0) {
                        sec_fmt(buf, sizeof(buf), si + 1, s);
                        clr = color_for(s, best_in_session(last_idx));
                    } else {
                        strncpy(buf, "---", sizeof(buf));
                    }
                } else {
                    strncpy(buf, "---", sizeof(buf));
                }
                lv_obj_set_style_text_color(lbl, clr, 0);
                lbl_set(lbl, buf);
                return;
            }
            case FIELD_RPM:
                snprintf(buf, sizeof(buf), "%.0f rpm", obd.rpm);
                break;
            case FIELD_THROTTLE:
                snprintf(buf, sizeof(buf), "%.0f %%", obd.throttle_pct);
                break;
            case FIELD_BOOST: {
                float boost = obd.boost_kpa - 101.3f;
                snprintf(buf, sizeof(buf), "%+.0f kPa", boost);
                break;
            }
            case FIELD_COOLANT:
                snprintf(buf, sizeof(buf), "%.0f °C", obd.coolant_temp_c);
                break;
            case FIELD_INTAKE:
                snprintf(buf, sizeof(buf), "%.0f °C", obd.intake_temp_c);
                break;
            case FIELD_LAMBDA:
                snprintf(buf, sizeof(buf), "%.2f \xCE\xBB", obd.lambda);
                break;
            case FIELD_BRAKE:
                snprintf(buf, sizeof(buf), "%.0f %%", obd.brake_pct);
                break;
            case FIELD_STEERING:
                snprintf(buf, sizeof(buf), "%+.0f°", obd.steering_angle);
                break;
            // Analog inputs — value already calibrated by analog_in_poll()
            case FIELD_AN1:
            case FIELD_AN2:
            case FIELD_AN3:
            case FIELD_AN4: {
                int an = fid - FIELD_AN1;
                const AnalogChannel &a = g_state.analog[an];
                if (!a.valid) {
                    strncpy(buf, "---", sizeof(buf));
                } else {
                    snprintf(buf, sizeof(buf), "%.2f", a.value);
                }
                break;
            }
            default:
                return;
        }
        lbl_set(lbl, buf);
    };

    if (timing_active) {
        for (int i = 0; i < Z1_SLOTS; i++) update_slot(tw.z1_val[i], g_dash_cfg.z1[i]);
        for (int i = 0; i < Z2_SLOTS; i++) update_slot(tw.z2_val[i], g_dash_cfg.z2[i]);
        for (int i = 0; i < Z3_SLOTS; i++) update_slot(tw.z3_val[i], g_dash_cfg.z3[i]);
    }

    if (timing_active && tw.delta_bar_fill && tw.delta_bar_lbl) {
        int32_t d     = g_state.timing.live_delta_ms;
        int32_t scale = timing_get_delta_scale();
        int16_t bh    = tw.delta_bar_h > 0 ? tw.delta_bar_h : 80;
        const int HALF = BRL_SCREEN_W / 2 - 4;
        int fill_w = (int)(fabsf((float)d) / (float)scale * HALF);
        if (fill_w > HALF) fill_w = HALF;
        // Purple bar replaces green when the driver is chasing the all-time
        // record (session hasn't beaten the stored best yet).
        extern bool g_chasing_record;
        lv_color_t faster_clr = g_chasing_record
            ? BRL_CLR_PURPLE
            : BRL_CLR_OK;
        lv_color_t slower_clr = g_chasing_record
            ? BRL_CLR_PURPLE_DIM
            : BRL_CLR_DANGER;
        if (d == 0 || !g_state.timing.timing_active) {
            lv_obj_set_size(tw.delta_bar_fill, 0, bh);
        } else if (d > 0) {
            lv_obj_set_style_bg_color(tw.delta_bar_fill, slower_clr, 0);
            lv_obj_set_size(tw.delta_bar_fill, fill_w, bh);
            lv_obj_set_pos(tw.delta_bar_fill, BRL_SCREEN_W / 2 - fill_w, 0);
        } else {
            lv_obj_set_style_bg_color(tw.delta_bar_fill, faster_clr, 0);
            lv_obj_set_size(tw.delta_bar_fill, fill_w, bh);
            lv_obj_set_pos(tw.delta_bar_fill, BRL_SCREEN_W / 2, 0);
        }
        char dbuf[16];
        if (!g_state.timing.timing_active || d == 0) {
            snprintf(dbuf, sizeof(dbuf), "\xC2\xB1" "0.00 s");
        } else {
            snprintf(dbuf, sizeof(dbuf), "%+.2f s", d / 1000.0f);
        }
        lbl_set(tw.delta_bar_lbl, dbuf);
    }

    // Track name
    if (timing_active && tw.track_name_lbl) {
        if (g_state.active_track_idx >= 0 && g_state.active_track_idx < track_total_count()) {
            lbl_set(tw.track_name_lbl,
                    track_get(g_state.active_track_idx)->name);
        }
    }

    // ---- GPS map widget update (once per second max) ----
    if (timing_active && tw.map_obj) {
        static uint32_t s_map_last_ms = 0;
        uint32_t now_ms = millis();
        if (now_ms - s_map_last_ms >= 1000) {
            s_map_last_ms = now_ms;

            const LapSession &sess = g_state.session;

            // Collect bounding box across ref trace, cur trace, current pos
            double mn_lat= 1e9, mx_lat=-1e9, mn_lon= 1e9, mx_lon=-1e9;
            bool   any = false;

            auto expand = [&](double lat, double lon) {
                if (lat < mn_lat) mn_lat = lat;
                if (lat > mx_lat) mx_lat = lat;
                if (lon < mn_lon) mn_lon = lon;
                if (lon > mx_lon) mx_lon = lon;
                any = true;
            };

            // Reference lap points
            const RecordedLap *ref = nullptr;
            if (sess.lap_count > 0 && sess.laps[sess.ref_lap_idx].valid)
                ref = &sess.laps[sess.ref_lap_idx];
            if (ref) {
                for (uint16_t i = 0; i < ref->point_count; i++)
                    expand(ref->points[i].lat, ref->points[i].lon);
            }

            // Current in-progress lap points
            uint16_t cur_n = 0;
            const TrackPoint *cur = lap_timer_get_cur_points(&cur_n);
            if (cur) {
                for (uint16_t i = 0; i < cur_n; i++)
                    expand(cur[i].lat, cur[i].lon);
            }

            // Current GPS position
            if (g_state.gps.valid) expand(g_state.gps.lat, g_state.gps.lon);

            if (!any) { g_map_data.valid = false; }
            else {
                double lat_r = mx_lat - mn_lat;
                double lon_r = mx_lon - mn_lon;
                // Ensure minimum range to avoid division by zero
                if (lat_r < 1e-6) lat_r = 1e-6;
                if (lon_r < 1e-6) lon_r = 1e-6;
                // 5 % padding
                const double PAD = 0.05;
                mn_lat -= lat_r * PAD; mx_lat += lat_r * PAD; lat_r *= 1.0 + 2*PAD;
                mn_lon -= lon_r * PAD; mx_lon += lon_r * PAD; lon_r *= 1.0 + 2*PAD;

                // Normalize: lon → x, lat → y (inverted, north=top)
                auto norm = [&](double lat, double lon, float &nx, float &ny) {
                    nx = (float)((lon - mn_lon) / lon_r);
                    ny = 1.0f - (float)((lat - mn_lat) / lat_r);
                };

                // Build ref trace (downsample to MAP_MAX_PTS)
                g_map_data.ref_n = 0;
                if (ref && ref->point_count > 0) {
                    int step = (int)ref->point_count / MAP_MAX_PTS;
                    if (step < 1) step = 1;
                    for (uint16_t i = 0; i < ref->point_count && g_map_data.ref_n < MAP_MAX_PTS; i += step) {
                        norm(ref->points[i].lat, ref->points[i].lon,
                             g_map_data.ref[g_map_data.ref_n].x,
                             g_map_data.ref[g_map_data.ref_n].y);
                        g_map_data.ref_n++;
                    }
                }

                // Build cur trace (downsample to MAP_MAX_PTS)
                g_map_data.cur_n = 0;
                if (cur && cur_n > 0) {
                    int step = (int)cur_n / MAP_MAX_PTS;
                    if (step < 1) step = 1;
                    for (uint16_t i = 0; i < cur_n && g_map_data.cur_n < MAP_MAX_PTS; i += step) {
                        norm(cur[i].lat, cur[i].lon,
                             g_map_data.cur[g_map_data.cur_n].x,
                             g_map_data.cur[g_map_data.cur_n].y);
                        g_map_data.cur_n++;
                    }
                }

                // Current position dot
                if (g_state.gps.valid) {
                    norm(g_state.gps.lat, g_state.gps.lon, g_map_data.pos_x, g_map_data.pos_y);
                    g_map_data.has_pos = true;
                } else {
                    g_map_data.has_pos = false;
                }

                g_map_data.valid = true;
                lv_obj_invalidate(tw.map_obj);
            }
        }
    }

    // ---- Settings screen labels (null-checked) ----
    // Gate entire block behind active-screen check: settings labels only
    // exist while s_scr_sub is the settings screen, and only need refresh
    // when the user is actually looking at them.
    const bool settings_active = on_active(set_obd_status_lbl)
                              || on_active(set_wifi_sta_status_lbl)
                              || on_active(set_obd_btn);
    if (settings_active && set_obd_status_lbl) {
        bool conn = obd_is_connected();
        lv_label_set_text(set_obd_status_lbl,
                          conn ? tr(TR_CONNECTED)
                               : (obd_is_scanning() ? tr(TR_SCANNING) : tr(TR_NOT_CONNECTED)));
        lv_obj_set_style_text_color(set_obd_status_lbl,
            conn ? lv_color_hex(0x00CC66) : lv_color_hex(0xAAAAAA), 0);
    }
    if (settings_active && set_obd_btn) {
        bool conn = obd_is_connected();
        char obd_btn_buf[48];
        snprintf(obd_btn_buf, sizeof(obd_btn_buf),
                 conn ? LV_SYMBOL_CLOSE " %s" : LV_SYMBOL_BLUETOOTH " %s",
                 conn ? tr(TR_DISCONNECT_BTN) : tr(TR_CONNECT_BTN));
        lv_label_set_text(lv_obj_get_child(set_obd_btn, 0), obd_btn_buf);
    }
    // AP is always active — status label is a static green "Aktiv", no dynamic update needed.
    if (settings_active && set_wifi_sta_status_lbl) {
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
    bool sd_was_available = sd_mgr_available(); // preserve across g_state reset
    WifiMode wifi_was    = g_state.wifi_mode;   // preserve — set by wifi_mgr_init()
    char wifi_ssid_was[sizeof(g_state.wifi_ssid)];
    memcpy(wifi_ssid_was, g_state.wifi_ssid, sizeof(wifi_ssid_was));
    g_state = {};
    g_state.active_track_idx = -1;
    g_state.sd_available     = sd_was_available;
    g_state.wifi_mode        = wifi_was;
    memcpy(g_state.wifi_ssid, wifi_ssid_was, sizeof(g_state.wifi_ssid));

    dash_config_load();
    g_state.language      = g_dash_cfg.language;
    g_state.units         = g_dash_cfg.units;
    g_state.veh_conn_mode = (VehicleConnMode)g_dash_cfg.veh_conn_mode;
    i18n_set_language(g_state.language);

    // Create the persistent menu screen now (so timing_screen_build can
    // reference menu_screen_show even before the menu is visible).
    build_menu_screen();

    // Show splash, then load menu screen when done.
    // (L2-cache warm-up happens continuously via timer_live_update, which
    // force-invalidates non-timing screens every ~400 ms — that's what
    // keeps the first scroll gesture smooth.)
    splash_show(3000, []() {
        lv_screen_load(s_scr_menu);
    });
}

void app_tick() {
    // Reserved for future per-loop work (currently handled by lv_timer_handler)
}
