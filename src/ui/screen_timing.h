#pragma once
#include <lvgl.h>
#include <stdint.h>
#include "dash_config.h"

// ---------------------------------------------------------------------------
// GPS map data — updated by timer_live_update, consumed by map draw callback.
// Normalized coords: x/y in [0..1], origin = top-left, y increases downward.
// ---------------------------------------------------------------------------
#define MAP_MAX_PTS 500

struct MapDisplayData {
    struct Pt { float x, y; };
    Pt   ref[MAP_MAX_PTS];   // reference (best) lap trace — grey
    int  ref_n;
    Pt   cur[MAP_MAX_PTS];   // current in-progress lap trace — white
    int  cur_n;
    float pos_x, pos_y;      // current GPS position (normalized)
    bool  has_pos;
    bool  valid;             // false = no data / no bounding box yet
};
extern MapDisplayData g_map_data;

// ---------------------------------------------------------------------------
// Handles for timing-screen LVGL labels — one entry per configurable slot.
// z1_val / z2_val / z3_val: value label for each slot (null if FIELD_NONE or map).
// z1_title / z2_title / z3_title: title label (small text at top of card).
// Zero the whole struct BEFORE deleting the screen (timer safety).
// ---------------------------------------------------------------------------
typedef struct {
    lv_obj_t *z1_val  [Z1_SLOTS];
    lv_obj_t *z1_title[Z1_SLOTS];
    lv_obj_t *z2_val  [Z2_SLOTS];
    lv_obj_t *z2_title[Z2_SLOTS];
    lv_obj_t *z3_val  [Z3_SLOTS];
    lv_obj_t *z3_title[Z3_SLOTS];
    // Delta bar (fixed — not user-configurable)
    lv_obj_t *delta_bar_fill;
    lv_obj_t *delta_bar_lbl;
    int16_t   delta_bar_h;
    // Status bar labels
    lv_obj_t *sb_gps_lbl, *sb_obd_lbl;
    // Header
    lv_obj_t *track_name_lbl;
    // GPS map widget (shown when a slot has FIELD_MAP)
    lv_obj_t *map_obj;
} TimingWidgets;

extern TimingWidgets tw;

// Current delta bar scale in ms (cycles on tap: 2000/3000/5000/10000/20000)
int32_t timing_get_delta_scale();

// Build timing LVGL screen; returns the new screen object (NOT yet loaded).
lv_obj_t *timing_screen_build();

// Delete + rebuild + lv_screen_load.
void timing_screen_rebuild();

// Build (if needed) and load the timing screen.
void timing_screen_open();
