#pragma once
#include <lvgl.h>
#include <stdint.h>

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
// Handles for all timing-screen LVGL labels.
// Null if widget is disabled or screen is not yet built.
// Zero the whole struct BEFORE deleting the screen (timer safety).
// ---------------------------------------------------------------------------
typedef struct {
    lv_obj_t *speed_lbl;
    lv_obj_t *laptime_lbl;
    lv_obj_t *bestlap_lbl;
    lv_obj_t *delta_lbl;
    lv_obj_t *lap_nr_lbl;
    lv_obj_t *sec1_lbl, *sec2_lbl, *sec3_lbl;
    lv_obj_t *rpm_lbl, *throttle_lbl, *boost_lbl;
    lv_obj_t *lambda_lbl, *brake_lbl, *coolant_lbl;
    lv_obj_t *gear_lbl, *steering_lbl;
    lv_obj_t *start_btn_lbl;
    lv_obj_t *track_name_lbl;
    // Delta bar (horizontal bar at top of content area)
    lv_obj_t *delta_bar_fill;
    lv_obj_t *delta_bar_lbl;
    int16_t   delta_bar_h;   // actual pixel height (for use in timer update)
    // Status bar labels on the timing screen
    lv_obj_t *sb_gps_lbl, *sb_wifi_lbl, *sb_obd_lbl;
    // GPS map widget (draw-event driven)
    lv_obj_t *map_obj;
} TimingWidgets;

extern TimingWidgets tw;

// Current delta bar scale in ms (cycles on tap: 2000/3000/5000/10000/20000)
int32_t timing_get_delta_scale();

// Build timing LVGL screen; returns the new screen object (NOT yet loaded).
lv_obj_t *timing_screen_build();

// Delete + rebuild + lv_screen_load (called after layout editor saves).
void timing_screen_rebuild();

// Build (if needed) and load the timing screen.
void timing_screen_open();
