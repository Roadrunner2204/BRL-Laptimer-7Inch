/**
 * screen_timing.cpp — Timing screen with configurable data slots
 *
 * Layout (1024×600):
 *   y=  0  Status bar  (40 px)
 *   y= 40  Header bar  (50 px): ← MENÜ | track name | START/STOP
 *   y= 90  Delta bar   (80 px, fixed — not configurable)
 *   y=176  Zone 1     (≈140 px): 5 slots — 0-1 wide (390 px), 2-4 narrow (193 px)
 *   y=322  Zone 2     (≈ 85 px): 3 equal-width sector slots
 *   y=413  Zone 3     (≈ 60 px): 5 equal-width OBD slots
 *
 * Tapping any slot card opens a field-picker popup filtered to:
 *   Zone 1 & 2 → laptimer / GPS fields only
 *   Zone 3     → OBD fields only
 */

#include "screen_timing.h"
#include "brl_fonts.h"
#include "dash_config.h"
#include "theme.h"
#include "i18n.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include "../timing/lap_timer.h"
#include "../obd/obd_bt.h"
#include "../wifi/wifi_mgr.h"
#include "../storage/session_store.h"
#include "compat.h"
static const char *TAG = "screen_timing";

// ---------------------------------------------------------------------------
// Provided by app.cpp
// ---------------------------------------------------------------------------
extern void menu_screen_show();

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
TimingWidgets     tw = {};
static lv_obj_t  *s_timing_screen  = nullptr;
static lv_obj_t  *s_picker_overlay = nullptr;
static lv_obj_t  *s_scale_overlay  = nullptr;  // delta-bar scale picker

// Session-name dialog state
static lv_obj_t   *s_name_dlg       = nullptr;
static lv_obj_t   *s_name_ta        = nullptr;
static lv_obj_t   *s_name_kb        = nullptr;
static lv_obj_t   *s_name_count_lbl = nullptr;
static lv_timer_t *s_name_tmr       = nullptr;
static int         s_name_secs      = 8;
static bool        s_session_begun  = false;
static int         s_last_track_idx = -2;  // track last opened; -2 = never

// Delta bar scale — persists across screen rebuilds
static int32_t    s_delta_scale_ms = 3000;  // ±3 s default

// ── QWERTZ keyboard maps (static lifetime required by LVGL) ─────────────────
static const char * const s_qwertz_lc[] = {
    "1","2","3","4","5","6","7","8","9","0",LV_SYMBOL_BACKSPACE,"\n",
    "q","w","e","r","t","z","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l",LV_SYMBOL_NEW_LINE,"\n",
    "\xef\x84\xaa","y","x","c","v","b","n","m",".",",","\xef\x84\xaa","\n",
    "1#"," ",LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,""
};
static const char * const s_qwertz_uc[] = {
    "1","2","3","4","5","6","7","8","9","0",LV_SYMBOL_BACKSPACE,"\n",
    "Q","W","E","R","T","Z","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L",LV_SYMBOL_NEW_LINE,"\n",
    "\xef\x84\xaa","Y","X","C","V","B","N","M",".",",","\xef\x84\xaa","\n",
    "abc"," ",LV_SYMBOL_LEFT,LV_SYMBOL_RIGHT,""
};
// ctrl: same structure for LC and UC (widths unchanged between modes)
#define _K(w) static_cast<lv_btnmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS|(w))
static const lv_btnmatrix_ctrl_t s_qwertz_ctrl[] = {
    // Row 1: 10 digits + Backspace(wide)
    _K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(7),
    // Row 2: 10 letters
    _K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),
    // Row 3: 9 letters + Enter(wide)
    _K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(7),
    // Row 4: Shift(wide) + 9 letters/punct + Shift(wide)
    _K(7),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(5),_K(7),
    // Row 5: 1# + Space(wide) + Left + Right
    _K(5),_K(7),_K(5),_K(5),
};
#undef _K

int32_t timing_get_delta_scale() { return s_delta_scale_ms; }

// ---------------------------------------------------------------------------
// Field metadata
// ---------------------------------------------------------------------------
static const char *field_title(uint8_t f) {
    switch (f) {
        case FIELD_SPEED:     return tr(TR_SPEED);
        case FIELD_LAPTIME:   return tr(TR_LAPTIME);
        case FIELD_BESTLAP:   return tr(TR_BESTLAP);
        case FIELD_DELTA_NUM: return tr(TR_LIVE_DELTA);
        case FIELD_LAP_NR:    return tr(TR_LAP);
        case FIELD_SECTOR1:   return tr(TR_SECTOR1);
        case FIELD_SECTOR2:   return tr(TR_SECTOR2);
        case FIELD_SECTOR3:   return tr(TR_SECTOR3);
        case FIELD_MAP:       return "MAP";
        case FIELD_RPM:       return tr(TR_RPM);
        case FIELD_THROTTLE:  return tr(TR_THROTTLE);
        case FIELD_BOOST:     return tr(TR_BOOST);
        case FIELD_LAMBDA:    return tr(TR_LAMBDA);
        case FIELD_BRAKE:     return tr(TR_BRAKE);
        case FIELD_COOLANT:   return tr(TR_COOLANT);
        case FIELD_INTAKE:    return tr(TR_INTAKE);
        case FIELD_STEERING:  return tr(TR_STEERING);
        default:              return "---";
    }
}

// Value font + color per field
static const lv_font_t *field_font(uint8_t f, int zone, bool wide) {
    // Zone 1: always large — wide slots get 64, narrow get 48
    if (zone == 1) return wide ? &BRL_FONT_64 : &BRL_FONT_48;
    switch (f) {
        case FIELD_SECTOR1:
        case FIELD_SECTOR2:
        case FIELD_SECTOR3:   return &BRL_FONT_24;
        default:              return &BRL_FONT_20;
    }
}

static lv_color_t field_color(uint8_t f) {
    switch (f) {
        case FIELD_LAPTIME:   return BRL_CLR_ACCENT;
        case FIELD_DELTA_NUM: return BRL_CLR_TEXT_DIM;
        case FIELD_BOOST:     return BRL_CLR_WARN;
        case FIELD_BRAKE:     return BRL_CLR_DANGER;
        default:              return BRL_CLR_TEXT;
    }
}

// ---------------------------------------------------------------------------
// GPS map draw callback (LVGL 9 layer-based draw API)
// ---------------------------------------------------------------------------
MapDisplayData g_map_data = {};

static void cb_map_draw(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = (lv_obj_t*)lv_event_get_target(e);

    lv_area_t a;
    lv_obj_get_content_coords(obj, &a);
    int32_t W = a.x2 - a.x1 + 1;
    int32_t H = a.y2 - a.y1 + 1;
    if (W <= 0 || H <= 0) return;

    auto px = [&](float nx) -> float { return (float)a.x1 + nx * (float)W; };
    auto py = [&](float ny) -> float { return (float)a.y1 + ny * (float)H; };

    if (!g_map_data.valid || (g_map_data.ref_n < 2 && !g_map_data.has_pos)) {
        lv_draw_label_dsc_t ldsc;
        lv_draw_label_dsc_init(&ldsc);
        ldsc.color = lv_color_hex(0x555555);
        ldsc.font  = &BRL_FONT_14;
        ldsc.text = "GPS...";
        lv_area_t ta = a;
        lv_draw_label(layer, &ldsc, &ta);
        return;
    }

    // Reference trace — grey
    if (g_map_data.ref_n > 1) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x3A3A3A);
        dsc.width = 2;
        for (int i = 1; i < g_map_data.ref_n; i++) {
            dsc.p1.x = px(g_map_data.ref[i-1].x);
            dsc.p1.y = py(g_map_data.ref[i-1].y);
            dsc.p2.x = px(g_map_data.ref[i].x);
            dsc.p2.y = py(g_map_data.ref[i].y);
            lv_draw_line(layer, &dsc);
        }
    }

    // Current lap trace — white
    if (g_map_data.cur_n > 1) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0xCCCCCC);
        dsc.width = 2;
        for (int i = 1; i < g_map_data.cur_n; i++) {
            dsc.p1.x = px(g_map_data.cur[i-1].x);
            dsc.p1.y = py(g_map_data.cur[i-1].y);
            dsc.p2.x = px(g_map_data.cur[i].x);
            dsc.p2.y = py(g_map_data.cur[i].y);
            lv_draw_line(layer, &dsc);
        }
    }

    // Current GPS position dot
    if (g_map_data.has_pos) {
        int32_t cx = (int32_t)px(g_map_data.pos_x);
        int32_t cy = (int32_t)py(g_map_data.pos_y);
        lv_draw_rect_dsc_t rdsc;
        lv_draw_rect_dsc_init(&rdsc);
        rdsc.bg_color = lv_color_hex(0x0096FF);
        rdsc.bg_opa   = LV_OPA_50;
        rdsc.radius   = LV_RADIUS_CIRCLE;
        rdsc.border_width = 0;
        rdsc.shadow_width = 0;
        lv_area_t ga = {cx-8, cy-8, cx+8, cy+8};
        lv_draw_rect(layer, &rdsc, &ga);
        rdsc.bg_color = lv_color_white();
        rdsc.bg_opa   = LV_OPA_COVER;
        lv_area_t da = {cx-4, cy-4, cx+4, cy+4};
        lv_draw_rect(layer, &rdsc, &da);
    }
}

// ---------------------------------------------------------------------------
// Field picker popup
// ---------------------------------------------------------------------------

// Encode zone+slot into a single pointer-sized token
#define PICKER_TOKEN(zone, slot) ((void*)(intptr_t)(((zone) << 8) | (slot)))
#define PICKER_ZONE(tok)         (((int)(intptr_t)(tok)) >> 8)
#define PICKER_SLOT(tok)         (((int)(intptr_t)(tok)) & 0xFF)

static void close_picker() {
    if (s_picker_overlay) {
        lv_obj_delete(s_picker_overlay);
        s_picker_overlay = nullptr;
    }
}

// Called when user taps a field button in the picker
static void cb_pick_field(lv_event_t *e) {
    // user_data on the button encodes the FieldId
    uint8_t fid = (uint8_t)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    // zone+slot encoded in the event user_data (via lv_obj_add_event_cb)
    int token = (int)(intptr_t)lv_event_get_user_data(e);
    int zone  = token >> 8;
    int slot  = token & 0xFF;

    switch (zone) {
        case 1: g_dash_cfg.z1[slot] = fid; break;
        case 2: g_dash_cfg.z2[slot] = fid; break;
        case 3: g_dash_cfg.z3[slot] = fid; break;
    }
    dash_config_save();
    close_picker();
    timing_screen_rebuild();
}

// Laptimer-side field list (Zone 1 & 2)
static const uint8_t LAPTIME_FIELDS[] = {
    FIELD_SPEED, FIELD_LAPTIME, FIELD_BESTLAP,
    FIELD_LAP_NR, FIELD_SECTOR1, FIELD_SECTOR2, FIELD_SECTOR3,
    FIELD_MAP, FIELD_NONE,
};
// OBD field list (Zone 3)
static const uint8_t OBD_FIELDS[] = {
    FIELD_RPM, FIELD_THROTTLE, FIELD_BOOST, FIELD_COOLANT,
    FIELD_INTAKE, FIELD_LAMBDA, FIELD_BRAKE, FIELD_STEERING,
    FIELD_NONE,
};

static void open_field_picker(int zone, int slot, uint8_t current_field) {
    if (!s_timing_screen) return;
    close_picker();  // dismiss any existing picker

    s_picker_overlay = lv_obj_create(s_timing_screen);
    lv_obj_set_size(s_picker_overlay, BRL_SCREEN_W, BRL_SCREEN_H);
    lv_obj_set_pos(s_picker_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_picker_overlay, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_picker_overlay, LV_OPA_80, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_picker_overlay, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_picker_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Tap backdrop to dismiss
    lv_obj_add_flag(s_picker_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_picker_overlay, [](lv_event_t * /*e*/) { close_picker(); },
                        LV_EVENT_CLICKED, nullptr);

    // Card
    lv_obj_t *card = lv_obj_create(s_picker_overlay);
    lv_obj_set_size(card, 680, 260);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    brl_style_card(card);
    lv_obj_set_style_pad_all(card, 12, LV_STATE_DEFAULT);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Prevent clicks from reaching backdrop
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t *ev) { lv_event_stop_bubbling(ev); },
                        LV_EVENT_CLICKED, nullptr);

    // Title
    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text_fmt(ttl, "Datenfeld wählen (Zone %d · Slot %d)", zone, slot + 1);
    brl_style_label(ttl, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Button grid
    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_size(grid, 656, 190);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    brl_style_transparent(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(grid, 6, LV_STATE_DEFAULT);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    const uint8_t *fields = (zone == 3) ? OBD_FIELDS : LAPTIME_FIELDS;
    int n_fields = (zone == 3) ? (int)sizeof(OBD_FIELDS) : (int)sizeof(LAPTIME_FIELDS);

    int token = (zone << 8) | slot;

    for (int i = 0; i < n_fields; i++) {
        uint8_t fid = fields[i];
        lv_obj_t *btn = lv_button_create(grid);
        lv_obj_set_size(btn, 148, 50);
        bool active = (fid == current_field);
        brl_style_btn(btn, active ? BRL_CLR_ACCENT : BRL_CLR_SURFACE2);
        lv_obj_set_user_data(btn, (void*)(intptr_t)fid);
        lv_obj_add_event_cb(btn, cb_pick_field, LV_EVENT_CLICKED, (void*)(intptr_t)token);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, fid == FIELD_NONE ? "AUS" : field_title(fid));
        brl_style_label(lbl, &BRL_FONT_14, BRL_CLR_TEXT);
        lv_obj_center(lbl);
    }
}

// Slot click callback — zone+slot encoded in user_data of card object
static void cb_slot_click(lv_event_t *e) {
    lv_obj_t *card = (lv_obj_t*)lv_event_get_current_target(e);
    int token = (int)(intptr_t)lv_obj_get_user_data(card);
    int zone  = token >> 8;
    int slot  = token & 0xFF;
    uint8_t cur = 0;
    switch (zone) {
        case 1: cur = g_dash_cfg.z1[slot]; break;
        case 2: cur = g_dash_cfg.z2[slot]; break;
        case 3: cur = g_dash_cfg.z3[slot]; break;
    }
    open_field_picker(zone, slot, cur);
}

// ---------------------------------------------------------------------------
// Card builder helper
// ---------------------------------------------------------------------------
static lv_obj_t *mk_row(lv_obj_t *parent, int y, int h) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, 784, h);
    lv_obj_set_pos(r, 8, y);
    brl_style_transparent(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 4, LV_STATE_DEFAULT);
    return r;
}

// Build one slot card. Returns the value label (or null for MAP / NONE).
// title_out receives the title label so it can be updated on rebuild.
static lv_obj_t *mk_slot_card(lv_obj_t *row, int zone, int slot_idx,
                               uint8_t fid, int w, int h,
                               lv_obj_t **title_out) {
    lv_obj_t *c = lv_obj_create(row);
    lv_obj_set_size(c, w, h);
    brl_style_card(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(c, PICKER_TOKEN(zone, slot_idx));
    lv_obj_add_event_cb(c, cb_slot_click, LV_EVENT_CLICKED, nullptr);
    // Pressed highlight
    lv_obj_set_style_bg_color(c, BRL_CLR_SURFACE2, LV_STATE_PRESSED);

    // Title label
    lv_obj_t *t = lv_label_create(c);
    lv_label_set_text(t, fid == FIELD_NONE ? "---" : field_title(fid));
    brl_style_label(t, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    if (title_out) *title_out = t;

    if (fid == FIELD_NONE) return nullptr;

    if (fid == FIELD_MAP) {
        // Map widget fills the card — must NOT consume clicks so picker still opens
        tw.map_obj = lv_obj_create(c);
        lv_obj_set_size(tw.map_obj, w - 16, h - 32);
        lv_obj_align(tw.map_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
        brl_style_transparent(tw.map_obj);
        lv_obj_remove_flag(tw.map_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(tw.map_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tw.map_obj, cb_map_draw, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_add_event_cb(tw.map_obj, cb_map_draw, LV_EVENT_DRAW_POST, nullptr);
        return nullptr;
    }

    // Value label
    bool wide = (zone == 1 && slot_idx <= 1);
    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "---");
    brl_style_label(v, field_font(fid, zone, wide), field_color(fid));
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, 0);
    return v;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Session-name dialog
// ---------------------------------------------------------------------------
static void name_dlg_confirm() {
    if (!s_name_dlg) return;

    // Cancel countdown timer
    if (s_name_tmr) { lv_timer_delete(s_name_tmr); s_name_tmr = nullptr; }

    // Read name from text area; fall back to default if empty
    const char *entered = s_name_ta ? lv_textarea_get_text(s_name_ta) : nullptr;
    char name[64];
    if (!entered || strlen(entered) == 0) {
        session_store_make_default_name(name, sizeof(name));
    } else {
        strncpy(name, entered, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    // Begin session with chosen name
    const TrackDef *td = track_get(g_state.active_track_idx);
    session_store_begin(td ? td->name : "Unknown", name);
    s_session_begun = true;

    // Close dialog
    lv_obj_delete(s_name_dlg);
    s_name_dlg = s_name_ta = s_name_kb = s_name_count_lbl = nullptr;
}

static void name_countdown_cb(lv_timer_t * /*t*/) {
    s_name_secs--;
    if (s_name_count_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Auto-Start in %ds", s_name_secs);
        lv_label_set_text(s_name_count_lbl, buf);
    }
    if (s_name_secs <= 0) name_dlg_confirm();
}

static void timing_show_session_name_dialog() {
    if (s_session_begun || !s_timing_screen) return;

    // Default name prefilled from GPS time
    char def_name[64];
    session_store_make_default_name(def_name, sizeof(def_name));
    s_name_secs = 8;

    // ── Full-screen dimmed overlay (no padding, no radius) ──────────────────
    s_name_dlg = lv_obj_create(s_timing_screen);
    lv_obj_set_size(s_name_dlg, BRL_SCREEN_W, BRL_SCREEN_H);
    lv_obj_set_pos(s_name_dlg, 0, 0);
    lv_obj_set_style_bg_color(s_name_dlg, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_name_dlg, LV_OPA_70, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_name_dlg, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_name_dlg, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_name_dlg, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_name_dlg, LV_OBJ_FLAG_SCROLLABLE);

    // ── Top card: title + textarea + buttons (slim, no keyboard inside) ─────
    // Height: 16 pad + 30 title + 8 gap + 50 ta + 8 gap + 46 btns + 16 pad = 174
    lv_obj_t *card = lv_obj_create(s_name_dlg);
    lv_obj_set_size(card, BRL_SCREEN_W, 174);
    lv_obj_set_pos(card, 0, 40);          // below status bar
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(card, lv_color_hex(0x333333), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_hor(card, 16, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_ver(card, 16, LV_STATE_DEFAULT);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title row: label + countdown
    lv_obj_t *title_row = lv_obj_create(card);
    lv_obj_set_size(title_row, LV_PCT(100), 30);
    lv_obj_set_pos(title_row, 0, 0);
    brl_style_transparent(title_row);
    lv_obj_remove_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, "Session benennen");
    brl_style_label(title, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    s_name_count_lbl = lv_label_create(title_row);
    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "Auto-Start in %ds", s_name_secs);
    lv_label_set_text(s_name_count_lbl, count_buf);
    brl_style_label(s_name_count_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(s_name_count_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // Text area with default name
    s_name_ta = lv_textarea_create(card);
    lv_obj_set_size(s_name_ta, LV_PCT(100), 50);
    lv_obj_set_pos(s_name_ta, 0, 38);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_text(s_name_ta, def_name);
    lv_obj_set_style_text_font(s_name_ta, &BRL_FONT_16, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_name_ta, lv_color_hex(0x111111), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_name_ta, lv_color_hex(0x00CC66), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_name_ta, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_name_ta, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    // Stop countdown the moment the user taps the textarea — no time pressure
    lv_obj_add_event_cb(s_name_ta, [](lv_event_t * /*e*/) {
        if (s_name_tmr) { lv_timer_delete(s_name_tmr); s_name_tmr = nullptr; }
        if (s_name_count_lbl) lv_label_set_text(s_name_count_lbl, "Manuell starten");
    }, LV_EVENT_FOCUSED, nullptr);

    // Button row
    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_set_size(btn_row, LV_PCT(100), 46);
    lv_obj_set_pos(btn_row, 0, 96);
    brl_style_transparent(btn_row);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    // "Starten" button
    lv_obj_t *btn_ok = lv_button_create(btn_row);
    lv_obj_set_size(btn_ok, 300, 40);
    lv_obj_align(btn_ok, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0x00CC66), LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_ok, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_ok, 0, LV_STATE_DEFAULT);
    lv_obj_t *ok_lbl = lv_label_create(btn_ok);
    lv_label_set_text(ok_lbl, LV_SYMBOL_OK "  Starten");
    brl_style_label(ok_lbl, &BRL_FONT_16, lv_color_hex(0x000000));
    lv_obj_center(ok_lbl);
    lv_obj_add_event_cb(btn_ok, [](lv_event_t * /*e*/) { name_dlg_confirm(); },
                        LV_EVENT_CLICKED, nullptr);

    // "Standard" button (skip → use default name)
    lv_obj_t *btn_skip = lv_button_create(btn_row);
    lv_obj_set_size(btn_skip, 220, 40);
    lv_obj_align(btn_skip, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_skip, lv_color_hex(0x222222), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn_skip, lv_color_hex(0x444444), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_skip, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_skip, 8, LV_STATE_DEFAULT);
    lv_obj_t *skip_lbl = lv_label_create(btn_skip);
    lv_label_set_text(skip_lbl, "Standard verwenden");
    brl_style_label(skip_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_center(skip_lbl);
    lv_obj_add_event_cb(btn_skip, [](lv_event_t * /*e*/) {
        // Clear TA so name_dlg_confirm fills in default
        if (s_name_ta) lv_textarea_set_text(s_name_ta, "");
        name_dlg_confirm();
    }, LV_EVENT_CLICKED, nullptr);

    // ── Keyboard: child of overlay (NOT card), full-width, anchored at bottom
    // Use LV_ALIGN_BOTTOM_LEFT so LVGL computes position from parent bounds,
    // which is more reliable than lv_obj_set_pos when the theme has implicit padding.
    s_name_kb = lv_keyboard_create(s_name_dlg);
    lv_keyboard_set_mode(s_name_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_map(s_name_kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_qwertz_lc, s_qwertz_ctrl);
    lv_keyboard_set_map(s_name_kb, LV_KEYBOARD_MODE_TEXT_UPPER, s_qwertz_uc, s_qwertz_ctrl);
    lv_keyboard_set_textarea(s_name_kb, s_name_ta);
    lv_obj_set_size(s_name_kb, BRL_SCREEN_W, 266);
    lv_obj_align(s_name_kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_name_kb, lv_color_hex(0x111111), LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_name_kb, LV_OBJ_FLAG_SCROLLABLE);

    // Countdown timer — fires every 1 second
    s_name_tmr = lv_timer_create(name_countdown_cb, 1000, nullptr);
}

// Build
// ---------------------------------------------------------------------------
static void cb_back(lv_event_t * /*e*/) { menu_screen_show(); }

lv_obj_t *timing_screen_build() {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Status bar (40 px) ────────────────────────────────────────────────
    lv_obj_t *sb = lv_obj_create(scr);
    lv_obj_set_size(sb, BRL_SCREEN_W, 40);
    lv_obj_set_pos(sb, 0, 0);
    lv_obj_set_style_bg_color(sb, BRL_CLR_STATUSBAR, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(sb, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    // ← MENÜ button — left side of status bar
    lv_obj_t *back_btn = lv_button_create(sb);
    lv_obj_set_size(back_btn, 88, 30);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 4, 0);
    brl_style_btn(back_btn, BRL_CLR_SURFACE2);
    lv_obj_t *blbl = lv_label_create(back_btn);
    lv_label_set_text_fmt(blbl, LV_SYMBOL_LEFT " %s", tr(TR_MENU_BTN));
    brl_style_label(blbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_center(blbl);
    lv_obj_add_event_cb(back_btn, cb_back, LV_EVENT_CLICKED, nullptr);

    // GPS satellites — left of center
    tw.sb_gps_lbl = lv_label_create(sb);
    lv_label_set_text(tw.sb_gps_lbl, LV_SYMBOL_GPS " 0");
    brl_style_label(tw.sb_gps_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(tw.sb_gps_lbl, 100, 13);

    // Track name — centered
    tw.track_name_lbl = lv_label_create(sb);
    const TrackDef *td = track_get(g_state.active_track_idx);
    lv_label_set_text(tw.track_name_lbl, td ? td->name : tr(TR_NO_TRACK));
    brl_style_label(tw.track_name_lbl, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_align(tw.track_name_lbl, LV_ALIGN_CENTER, 0, 0);

    // OBD — right side
    tw.sb_obd_lbl = lv_label_create(sb);
    lv_label_set_text(tw.sb_obd_lbl, LV_SYMBOL_BLUETOOTH " OBD --");
    brl_style_label(tw.sb_obd_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(tw.sb_obd_lbl, 700, 13);

    // ── Delta bar (80 px, fixed) ──────────────────────────────────────────
    const int DBAR_GAP = 6;
    const int DBAR_H   = 80;
    const int dbar_y   = 40 + DBAR_GAP;   // status bar is now 40 px, no separate header
    const int cy_start = dbar_y + DBAR_H + DBAR_GAP;

    tw.delta_bar_h = (int16_t)DBAR_H;

    lv_obj_t *dbar = lv_obj_create(scr);
    lv_obj_set_size(dbar, BRL_SCREEN_W, DBAR_H);
    lv_obj_set_pos(dbar, 0, dbar_y);
    lv_obj_set_style_bg_color(dbar, lv_color_hex(0x111111), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(dbar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dbar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(dbar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(dbar, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(dbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cmark = lv_obj_create(dbar);
    lv_obj_set_size(cmark, 2, DBAR_H);
    lv_obj_set_pos(cmark, 399, 0);
    lv_obj_set_style_bg_color(cmark, lv_color_hex(0x555555), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cmark, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cmark, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cmark, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(cmark, LV_OBJ_FLAG_SCROLLABLE);

    tw.delta_bar_fill = lv_obj_create(dbar);
    lv_obj_set_size(tw.delta_bar_fill, 0, DBAR_H);
    lv_obj_set_pos(tw.delta_bar_fill, 400, 0);
    lv_obj_set_style_bg_color(tw.delta_bar_fill, lv_color_hex(0x00CC66), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(tw.delta_bar_fill, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(tw.delta_bar_fill, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(tw.delta_bar_fill, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(tw.delta_bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    tw.delta_bar_lbl = lv_label_create(dbar);
    lv_label_set_text(tw.delta_bar_lbl, "\xC2\xB1" "0.00 s");
    brl_style_label(tw.delta_bar_lbl, &BRL_FONT_48, BRL_CLR_TEXT);
    lv_obj_align(tw.delta_bar_lbl, LV_ALIGN_CENTER, 0, 0);

    // Tap delta bar → scale picker
    lv_obj_add_flag(dbar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dbar, [](lv_event_t * /*e*/) {
        if (!s_timing_screen) return;
        if (s_scale_overlay) { lv_obj_delete(s_scale_overlay); s_scale_overlay = nullptr; return; }

        s_scale_overlay = lv_obj_create(s_timing_screen);
        lv_obj_set_size(s_scale_overlay, BRL_SCREEN_W, BRL_SCREEN_H);
        lv_obj_set_pos(s_scale_overlay, 0, 0);
        lv_obj_set_style_bg_color(s_scale_overlay, lv_color_hex(0x000000), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_scale_overlay, LV_OPA_50, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_scale_overlay, 0, LV_STATE_DEFAULT);
        lv_obj_remove_flag(s_scale_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_scale_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_scale_overlay, [](lv_event_t * /*e*/) {
            if (s_scale_overlay) { lv_obj_delete(s_scale_overlay); s_scale_overlay = nullptr; }
        }, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *card = lv_obj_create(s_scale_overlay);
        lv_obj_set_size(card, 360, 180);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
        brl_style_card(card);
        lv_obj_set_style_pad_all(card, 12, LV_STATE_DEFAULT);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t *ev) { lv_event_stop_bubbling(ev); },
                            LV_EVENT_CLICKED, nullptr);

        lv_obj_t *ttl = lv_label_create(card);
        lv_label_set_text(ttl, "Delta-Skala");
        brl_style_label(ttl, &BRL_FONT_16, BRL_CLR_TEXT);
        lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *row = lv_obj_create(card);
        lv_obj_set_size(row, 336, 80);
        lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
        brl_style_transparent(row);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 6, LV_STATE_DEFAULT);

        const int32_t scales[] = { 2000, 3000, 5000, 10000, 20000 };
        const char *labels[]   = { "\xC2\xB1" "2s", "\xC2\xB1" "3s", "\xC2\xB1" "5s",
                                   "\xC2\xB1" "10s", "\xC2\xB1" "20s" };
        for (int i = 0; i < 5; i++) {
            lv_obj_t *btn = lv_button_create(row);
            lv_obj_set_size(btn, 62, 70);
            bool active = (s_delta_scale_ms == scales[i]);
            brl_style_btn(btn, active ? BRL_CLR_ACCENT : BRL_CLR_SURFACE2);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, labels[i]);
            brl_style_label(lbl, &BRL_FONT_16, BRL_CLR_TEXT);
            lv_obj_center(lbl);
            lv_obj_set_user_data(btn, (void*)(intptr_t)scales[i]);
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                s_delta_scale_ms = (int32_t)(intptr_t)lv_obj_get_user_data(
                    (lv_obj_t*)lv_event_get_target(e));
                if (s_scale_overlay) { lv_obj_delete(s_scale_overlay); s_scale_overlay = nullptr; }
            }, LV_EVENT_CLICKED, nullptr);
        }
    }, LV_EVENT_CLICKED, nullptr);

    // ── Content zones ─────────────────────────────────────────────────────
    // Fixed heights: Z1=140, Z2=85, Z3=60 → total = 285, available = 480-cy_start-8
    const int avail_h = BRL_SCREEN_H - cy_start - 8;
    const int h3 = 72;   // taller so title label doesn't overlap value
    const int h2 = 85;
    const int h1 = avail_h - h2 - h3 - 12;   // 12 = 2×6 px gaps

    int cy = cy_start;

    // ── Zone 1 ────────────────────────────────────────────────────────────
    {
        lv_obj_t *row = mk_row(scr, cy, h1);
        const int ch = h1 - 10;
        const int WW = 390;    // wide slot
        const int NW = 193;    // narrow slot

        for (int s = 0; s < Z1_SLOTS; s++) {
            int w = (s <= 1) ? WW : NW;
            tw.z1_val[s] = mk_slot_card(row, 1, s, g_dash_cfg.z1[s], w, ch,
                                        &tw.z1_title[s]);
        }
        cy += h1 + 6;
    }

    // ── Zone 2 ────────────────────────────────────────────────────────────
    {
        lv_obj_t *row = mk_row(scr, cy, h2);
        const int sw = (784 - (Z2_SLOTS - 1) * 4) / Z2_SLOTS;
        const int sh = h2 - 10;

        for (int s = 0; s < Z2_SLOTS; s++) {
            tw.z2_val[s] = mk_slot_card(row, 2, s, g_dash_cfg.z2[s], sw, sh,
                                        &tw.z2_title[s]);
        }
        cy += h2 + 6;
    }

    // ── Zone 3 ────────────────────────────────────────────────────────────
    {
        lv_obj_t *row = mk_row(scr, cy, h3);
        const int ow = (784 - (Z3_SLOTS - 1) * 4) / Z3_SLOTS;
        const int oh = h3 - 10;

        for (int s = 0; s < Z3_SLOTS; s++) {
            tw.z3_val[s] = mk_slot_card(row, 3, s, g_dash_cfg.z3[s], ow, oh,
                                        &tw.z3_title[s]);
        }
    }

    return scr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void timing_screen_open() {
    if (!s_timing_screen) {
        tw = {};
        s_timing_screen = timing_screen_build();
    }
    // Reset session state whenever a different track is opened
    if (g_state.active_track_idx != s_last_track_idx) {
        s_session_begun  = false;
        s_last_track_idx = g_state.active_track_idx;
        // Remove any stale dialog from a previous visit
        if (s_name_dlg) {
            if (s_name_tmr) { lv_timer_delete(s_name_tmr); s_name_tmr = nullptr; }
            lv_obj_delete(s_name_dlg);
            s_name_dlg = s_name_ta = s_name_kb = s_name_count_lbl = nullptr;
        }
    }
    lv_screen_load(s_timing_screen);
    if (!s_session_begun) timing_show_session_name_dialog();
}

void timing_screen_rebuild() {
    // Kill countdown timer before overlay objects are freed
    if (s_name_tmr) { lv_timer_delete(s_name_tmr); s_name_tmr = nullptr; }
    s_name_dlg = s_name_ta = s_name_kb = s_name_count_lbl = nullptr;

    s_picker_overlay = nullptr;
    s_scale_overlay  = nullptr;
    s_session_begun  = false;   // new track selected → new session
    tw = {};
    if (s_timing_screen) {
        lv_obj_delete(s_timing_screen);
        s_timing_screen = nullptr;
    }
    s_timing_screen = timing_screen_build();
    lv_screen_load(s_timing_screen);
    timing_show_session_name_dialog();
}
