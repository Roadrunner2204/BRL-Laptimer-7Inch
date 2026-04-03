/**
 * screen_timing.cpp — Timing screen with configurable widget grid
 *
 * Layout (800×480):
 *   y=  0  Status bar  (40px)
 *   y= 40  Header bar  (50px): ← MENÜ | track name | START/STOP | ⚙
 *   y= 90  Zone 1      (140px): SPEED (wide), LAPTIME (wide), BESTLAP, DELTA, LAP_NR
 *   y=234  Zone 2      ( 85px): S1, S2, S3
 *   y=323  Zone 3      ( 75px): RPM, THROTTLE, BOOST, LAMBDA, BRAKE, COOLANT, GEAR, STEERING
 *   (zones hidden if all their widgets are disabled)
 */

#include "screen_timing.h"
#include "brl_fonts.h"
#include "dash_config.h"
#include "theme.h"
#include "../../include/i18n.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include "../timing/lap_timer.h"
#include "../obd/obd_bt.h"
#include "../wifi/wifi_mgr.h"
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Provided by app.cpp
// ---------------------------------------------------------------------------
extern void menu_screen_show();

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
TimingWidgets     tw = {};
static lv_obj_t  *s_timing_screen  = nullptr;
static lv_obj_t  *s_layout_overlay = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static lv_obj_t *mk_card(lv_obj_t *parent, int w, int h,
                          const char *title,
                          const lv_font_t *vfont, lv_color_t vcol,
                          lv_obj_t **val_out) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    brl_style_card(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(c);
    lv_label_set_text(t, title);
    brl_style_label(t, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, "---");
    brl_style_label(v, vfont, vcol);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (val_out) *val_out = v;
    return c;
}

// Row container — flex row, no scroll, transparent bg
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

// ---------------------------------------------------------------------------
// Layout editor
// ---------------------------------------------------------------------------
#define NUM_WIDGETS 16
static const TrKey WIDGET_NAME_KEYS[NUM_WIDGETS] = {
    TR_WNAME_SPEED,    TR_WNAME_LAPTIME, TR_WNAME_BESTLAP,  TR_WNAME_DELTA,
    TR_WNAME_LAPNR,    TR_WNAME_SEC1,    TR_WNAME_SEC2,     TR_WNAME_SEC3,
    TR_WNAME_RPM,      TR_WNAME_THROTTLE,TR_WNAME_BOOST,    TR_WNAME_LAMBDA,
    TR_WNAME_BRAKE,    TR_WNAME_COOLANT, TR_WNAME_GEAR,     TR_WNAME_STEERING,
};
static const uint32_t WIDGET_BITS[NUM_WIDGETS] = {
    WDGT_SPEED, WDGT_LAPTIME, WDGT_BESTLAP, WDGT_DELTA, WDGT_LAP_NR,
    WDGT_SECTOR1, WDGT_SECTOR2, WDGT_SECTOR3,
    WDGT_RPM, WDGT_THROTTLE, WDGT_BOOST, WDGT_LAMBDA,
    WDGT_BRAKE, WDGT_COOLANT, WDGT_GEAR, WDGT_STEERING,
};
static lv_obj_t *s_editor_cbs[NUM_WIDGETS] = {};

static void cb_layout_cancel(lv_event_t * /*e*/) {
    if (s_layout_overlay) {
        lv_obj_delete(s_layout_overlay);
        s_layout_overlay = nullptr;
    }
}

static void cb_layout_save(lv_event_t * /*e*/) {
    uint32_t mask = 0;
    for (int i = 0; i < NUM_WIDGETS; i++) {
        if (s_editor_cbs[i] && lv_obj_has_state(s_editor_cbs[i], LV_STATE_CHECKED)) {
            mask |= WIDGET_BITS[i];
        }
    }
    g_dash_cfg.visible_mask = mask ? mask : WDGT_DEFAULT_MASK;
    dash_config_save();
    s_layout_overlay = nullptr;
    timing_screen_rebuild();
}

static void open_layout_editor() {
    if (s_layout_overlay || !s_timing_screen) return;

    s_layout_overlay = lv_obj_create(s_timing_screen);
    lv_obj_set_size(s_layout_overlay, 800, 480);
    lv_obj_set_pos(s_layout_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_layout_overlay, lv_color_hex(0x000000), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_layout_overlay, LV_OPA_80, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_layout_overlay, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_layout_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_layout_overlay);
    lv_obj_set_size(card, 740, 400);
    lv_obj_center(card);
    brl_style_card(card);
    lv_obj_set_style_pad_all(card, 12, LV_STATE_DEFAULT);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text_fmt(ttl, LV_SYMBOL_SETTINGS "  %s", tr(TR_CUSTOMIZE_LAYOUT));
    brl_style_label(ttl, &BRL_FONT_20, BRL_CLR_TEXT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_size(grid, 716, 300);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, 36);
    brl_style_transparent(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(grid, 6, LV_STATE_DEFAULT);

    for (int i = 0; i < NUM_WIDGETS; i++) {
        lv_obj_t *cb = lv_checkbox_create(grid);
        lv_checkbox_set_text(cb, tr(WIDGET_NAME_KEYS[i]));
        lv_obj_set_width(cb, 340);
        lv_obj_set_style_text_font(cb, &BRL_FONT_14, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(cb, BRL_CLR_TEXT, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(cb, BRL_CLR_ACCENT,
            LV_STATE_CHECKED | LV_PART_INDICATOR);
        lv_obj_set_style_border_color(cb, BRL_CLR_BORDER,
            LV_PART_INDICATOR | LV_STATE_DEFAULT);
        if (g_dash_cfg.visible_mask & WIDGET_BITS[i])
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        s_editor_cbs[i] = cb;
    }

    lv_obj_t *btn_save = lv_button_create(card);
    lv_obj_set_size(btn_save, 150, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_save, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_save, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_save, 0, LV_STATE_DEFAULT);
    lv_obj_t *lsave = lv_label_create(btn_save);
    lv_label_set_text_fmt(lsave, LV_SYMBOL_OK "  %s", tr(TR_SAVE_BTN));
    brl_style_label(lsave, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(lsave);
    lv_obj_add_event_cb(btn_save, cb_layout_save, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_cancel = lv_button_create(card);
    lv_obj_set_size(btn_cancel, 130, 40);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_RIGHT, -160, 0);
    lv_obj_set_style_bg_color(btn_cancel, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn_cancel, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_cancel, 0, LV_STATE_DEFAULT);
    lv_obj_t *lcancel = lv_label_create(btn_cancel);
    lv_label_set_text_fmt(lcancel, LV_SYMBOL_CLOSE "  %s", tr(TR_CANCEL_BTN));
    brl_style_label(lcancel, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_center(lcancel);
    lv_obj_add_event_cb(btn_cancel, cb_layout_cancel, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------
static void cb_back(lv_event_t * /*e*/) { menu_screen_show(); }
static void cb_layout_btn(lv_event_t * /*e*/) { open_layout_editor(); }

static void cb_start_stop(lv_event_t * /*e*/) {
    LiveTiming &lt = g_state.timing;
    if (!lt.timing_active) {
        if (g_state.active_track_idx < 0) return;
        lt.timing_active = true;
        lt.lap_number    = 0;
        if (tw.start_btn_lbl)
            lv_label_set_text_fmt(tw.start_btn_lbl, LV_SYMBOL_STOP "  %s", tr(TR_STOP_BTN));
        lv_obj_t *btn = tw.start_btn_lbl ? lv_obj_get_parent(tw.start_btn_lbl) : nullptr;
        if (btn) lv_obj_set_style_bg_color(btn, BRL_CLR_DANGER, LV_STATE_DEFAULT);
    } else {
        lt.timing_active = false;
        if (tw.start_btn_lbl)
            lv_label_set_text_fmt(tw.start_btn_lbl, LV_SYMBOL_PLAY "  %s", tr(TR_START_BTN));
        lv_obj_t *btn = tw.start_btn_lbl ? lv_obj_get_parent(tw.start_btn_lbl) : nullptr;
        if (btn) lv_obj_set_style_bg_color(btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    }
}

lv_obj_t *timing_screen_build() {
    const uint32_t mask = g_dash_cfg.visible_mask;

    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, BRL_CLR_BG, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Status bar (40px) ──────────────────────────────────────────────────
    lv_obj_t *sb = lv_obj_create(scr);
    lv_obj_set_size(sb, 800, 40);
    lv_obj_set_pos(sb, 0, 0);
    lv_obj_set_style_bg_color(sb, BRL_CLR_STATUSBAR, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(sb, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(sb, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(sb, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    tw.sb_gps_lbl = lv_label_create(sb);
    lv_label_set_text(tw.sb_gps_lbl, LV_SYMBOL_GPS " 0");
    brl_style_label(tw.sb_gps_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(tw.sb_gps_lbl, 8, 12);

    tw.sb_wifi_lbl = lv_label_create(sb);
    lv_label_set_text(tw.sb_wifi_lbl, LV_SYMBOL_WIFI " --");
    brl_style_label(tw.sb_wifi_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(tw.sb_wifi_lbl, 120, 12);

    tw.sb_obd_lbl = lv_label_create(sb);
    lv_label_set_text(tw.sb_obd_lbl, LV_SYMBOL_BLUETOOTH " OBD --");
    brl_style_label(tw.sb_obd_lbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_set_pos(tw.sb_obd_lbl, 230, 12);

    // ── Header bar (50px) ──────────────────────────────────────────────────
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 800, 50);
    lv_obj_set_pos(hdr, 0, 40);
    lv_obj_set_style_bg_color(hdr, BRL_CLR_SURFACE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Back button
    lv_obj_t *back_btn = lv_button_create(hdr);
    lv_obj_set_size(back_btn, 100, 38);
    lv_obj_set_pos(back_btn, 6, 6);
    lv_obj_set_style_bg_color(back_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(back_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(back_btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *blbl = lv_label_create(back_btn);
    lv_label_set_text_fmt(blbl, LV_SYMBOL_LEFT "  %s", tr(TR_MENU_BTN));
    brl_style_label(blbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_center(blbl);
    lv_obj_add_event_cb(back_btn, cb_back, LV_EVENT_CLICKED, nullptr);

    // Track name (center)
    tw.track_name_lbl = lv_label_create(hdr);
    const TrackDef *td = track_get(g_state.active_track_idx);
    lv_label_set_text(tw.track_name_lbl, td ? td->name : tr(TR_NO_TRACK));
    brl_style_label(tw.track_name_lbl, &BRL_FONT_16, BRL_CLR_TEXT);
    lv_obj_align(tw.track_name_lbl, LV_ALIGN_CENTER, 0, 0);

    // Layout editor button
    lv_obj_t *layout_btn = lv_button_create(hdr);
    lv_obj_set_size(layout_btn, 120, 38);
    lv_obj_align(layout_btn, LV_ALIGN_RIGHT_MID, -140, 0);
    lv_obj_set_style_bg_color(layout_btn, BRL_CLR_SURFACE2, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(layout_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(layout_btn, 0, LV_STATE_DEFAULT);
    lv_obj_t *llbl = lv_label_create(layout_btn);
    lv_label_set_text_fmt(llbl, LV_SYMBOL_SETTINGS "  %s", tr(TR_LAYOUT_BTN));
    brl_style_label(llbl, &BRL_FONT_14, BRL_CLR_TEXT_DIM);
    lv_obj_center(llbl);
    lv_obj_add_event_cb(layout_btn, cb_layout_btn, LV_EVENT_CLICKED, nullptr);

    // Start/Stop button
    lv_obj_t *start_btn = lv_button_create(hdr);
    lv_obj_set_size(start_btn, 130, 38);
    lv_obj_align(start_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(start_btn, BRL_CLR_ACCENT, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(start_btn, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(start_btn, 0, LV_STATE_DEFAULT);
    tw.start_btn_lbl = lv_label_create(start_btn);
    lv_label_set_text_fmt(tw.start_btn_lbl,
        g_state.timing.timing_active
            ? LV_SYMBOL_STOP "  %s" : LV_SYMBOL_PLAY "  %s",
        g_state.timing.timing_active ? tr(TR_STOP_BTN) : tr(TR_START_BTN));
    brl_style_label(tw.start_btn_lbl, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_center(tw.start_btn_lbl);
    if (g_state.timing.timing_active)
        lv_obj_set_style_bg_color(start_btn, BRL_CLR_DANGER, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(start_btn, cb_start_stop, LV_EVENT_CLICKED, nullptr);

    // ── Delta bar (y=90, h=22) ─────────────────────────────────────────────
    lv_obj_t *dbar = lv_obj_create(scr);
    lv_obj_set_size(dbar, 800, 22);
    lv_obj_set_pos(dbar, 0, 90);
    lv_obj_set_style_bg_color(dbar, lv_color_hex(0x111111), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(dbar, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dbar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(dbar, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(dbar, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(dbar, LV_OBJ_FLAG_SCROLLABLE);

    // Center marker
    lv_obj_t *cmark = lv_obj_create(dbar);
    lv_obj_set_size(cmark, 2, 22);
    lv_obj_set_pos(cmark, 399, 0);
    lv_obj_set_style_bg_color(cmark, lv_color_hex(0x555555), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cmark, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cmark, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cmark, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(cmark, LV_OBJ_FLAG_SCROLLABLE);

    // Fill rectangle (width/position animated by timer)
    tw.delta_bar_fill = lv_obj_create(dbar);
    lv_obj_set_size(tw.delta_bar_fill, 0, 22);
    lv_obj_set_pos(tw.delta_bar_fill, 400, 0);
    lv_obj_set_style_bg_color(tw.delta_bar_fill, lv_color_hex(0x00CC66), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(tw.delta_bar_fill, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(tw.delta_bar_fill, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(tw.delta_bar_fill, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(tw.delta_bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    // Label (centered over bar)
    tw.delta_bar_lbl = lv_label_create(dbar);
    lv_label_set_text(tw.delta_bar_lbl, "\xC2\xB10.00 s");
    brl_style_label(tw.delta_bar_lbl, &BRL_FONT_14, BRL_CLR_TEXT);
    lv_obj_align(tw.delta_bar_lbl, LV_ALIGN_CENTER, 0, 0);

    // ── Zone 1: Large timing metrics (y=112, h=140) ───────────────────────
    bool z1 = (mask & (WDGT_SPEED | WDGT_LAPTIME | WDGT_BESTLAP | WDGT_DELTA | WDGT_LAP_NR));
    bool z2 = (mask & (WDGT_SECTOR1 | WDGT_SECTOR2 | WDGT_SECTOR3));
    bool z3 = (mask & (WDGT_RPM | WDGT_THROTTLE | WDGT_BOOST | WDGT_LAMBDA |
                       WDGT_BRAKE | WDGT_COOLANT | WDGT_GEAR | WDGT_STEERING));

    int zones = (z1 ? 1 : 0) + (z2 ? 1 : 0) + (z3 ? 1 : 0);
    int avail_h = 480 - 112 - 8;
    int h1 = 140, h2 = 85, h3 = 75;
    if (zones > 0) {
        int total_h = (z1 ? h1 : 0) + (z2 ? h2 : 0) + (z3 ? h3 : 0)
                    + (zones - 1) * 6;
        int slack = avail_h - total_h;
        if (z1 && slack > 0) h1 += slack;
    }

    int cy = 112;

    if (z1) {
        lv_obj_t *row1 = mk_row(scr, cy, h1);
        const int CW = 193;
        const int WW = 390;
        const int h  = h1 - 10;

        if (mask & WDGT_SPEED)
            mk_card(row1, WW, h, tr(TR_SPEED), &BRL_FONT_48,
                    BRL_CLR_TEXT, &tw.speed_lbl);
        if (mask & WDGT_LAPTIME)
            mk_card(row1, WW, h, tr(TR_LAPTIME), &BRL_FONT_40,
                    BRL_CLR_ACCENT, &tw.laptime_lbl);
        if (mask & WDGT_BESTLAP)
            mk_card(row1, CW, h, tr(TR_BESTLAP), &BRL_FONT_24,
                    BRL_CLR_TEXT, &tw.bestlap_lbl);
        if (mask & WDGT_DELTA)
            mk_card(row1, CW, h, tr(TR_LIVE_DELTA), &BRL_FONT_24,
                    BRL_CLR_TEXT_DIM, &tw.delta_lbl);
        if (mask & WDGT_LAP_NR)
            mk_card(row1, CW, h, tr(TR_LAP), &BRL_FONT_32,
                    BRL_CLR_TEXT, &tw.lap_nr_lbl);
        cy += h1 + 6;
    }

    if (z2) {
        lv_obj_t *row2 = mk_row(scr, cy, h2);
        int visible = ((mask & WDGT_SECTOR1) ? 1 : 0) +
                      ((mask & WDGT_SECTOR2) ? 1 : 0) +
                      ((mask & WDGT_SECTOR3) ? 1 : 0);
        int sw = visible ? (784 - (visible - 1) * 4) / visible : 258;
        int sh = h2 - 10;

        if (mask & WDGT_SECTOR1)
            mk_card(row2, sw, sh, tr(TR_SECTOR1), &BRL_FONT_24,
                    BRL_CLR_TEXT, &tw.sec1_lbl);
        if (mask & WDGT_SECTOR2)
            mk_card(row2, sw, sh, tr(TR_SECTOR2), &BRL_FONT_24,
                    BRL_CLR_TEXT, &tw.sec2_lbl);
        if (mask & WDGT_SECTOR3)
            mk_card(row2, sw, sh, tr(TR_SECTOR3), &BRL_FONT_24,
                    BRL_CLR_TEXT, &tw.sec3_lbl);
        cy += h2 + 6;
    }

    if (z3) {
        lv_obj_t *row3 = mk_row(scr, cy, h3);
        int visible = 0;
        uint32_t obd_bits[] = { WDGT_RPM, WDGT_THROTTLE, WDGT_BOOST, WDGT_LAMBDA,
                                 WDGT_BRAKE, WDGT_COOLANT, WDGT_GEAR, WDGT_STEERING };
        for (auto b : obd_bits) if (mask & b) visible++;
        int ow = visible ? (784 - (visible - 1) * 4) / visible : 93;
        int oh = h3 - 10;

        if (mask & WDGT_RPM)
            mk_card(row3, ow, oh, tr(TR_RPM), &BRL_FONT_20,
                    BRL_CLR_TEXT, &tw.rpm_lbl);
        if (mask & WDGT_THROTTLE)
            mk_card(row3, ow, oh, tr(TR_THROTTLE), &BRL_FONT_20,
                    BRL_CLR_TEXT, &tw.throttle_lbl);
        if (mask & WDGT_BOOST)
            mk_card(row3, ow, oh, tr(TR_BOOST), &BRL_FONT_20,
                    BRL_CLR_WARN, &tw.boost_lbl);
        if (mask & WDGT_LAMBDA)
            mk_card(row3, ow, oh, tr(TR_LAMBDA), &BRL_FONT_20,
                    BRL_CLR_TEXT, &tw.lambda_lbl);
        if (mask & WDGT_BRAKE)
            mk_card(row3, ow, oh, tr(TR_BRAKE), &BRL_FONT_20,
                    BRL_CLR_DANGER, &tw.brake_lbl);
        if (mask & WDGT_COOLANT)
            mk_card(row3, ow, oh, tr(TR_COOLANT), &BRL_FONT_20,
                    BRL_CLR_TEXT, &tw.coolant_lbl);
        if (mask & WDGT_GEAR)
            mk_card(row3, ow, oh, tr(TR_GEAR), &BRL_FONT_20,
                    BRL_CLR_TEXT, &tw.gear_lbl);
        if (mask & WDGT_STEERING)
            mk_card(row3, ow, oh, tr(TR_STEERING), &BRL_FONT_20,
                    BRL_CLR_TEXT, &tw.steering_lbl);
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
    lv_screen_load(s_timing_screen);
}

void timing_screen_rebuild() {
    s_layout_overlay = nullptr;
    tw = {};
    if (s_timing_screen) {
        lv_obj_delete(s_timing_screen);
        s_timing_screen = nullptr;
    }
    tw = {};
    s_timing_screen = timing_screen_build();
    lv_screen_load(s_timing_screen);
}
