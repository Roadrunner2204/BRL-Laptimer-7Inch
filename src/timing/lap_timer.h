#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../data/lap_data.h"
#include "../data/track_db.h"

/**
 * lap_timer — GPS-based automatic lap timing
 *
 * How it works:
 *   1. lap_timer_set_track() stores the active track's S/F + sector lines.
 *   2. lap_timer_poll() is called every GPS update (gps_poll()).
 *      It checks if the GPS path since last position crosses any timing line
 *      using a 2D line segment intersection test.
 *   3. On S/F crossing: finish previous lap, start new lap, record TrackPoint.
 *   4. On sector crossing: record sector split time.
 *   5. GPS track points are stored in a PSRAM-backed buffer.
 *
 * Line-crossing algorithm:
 *   We test if segment (prev_pos → curr_pos) intersects the timing line.
 *   This avoids false triggers from GPS jitter — must actually cross.
 *
 * Minimum lap time guard: 20 seconds (prevents double-triggers at slow speed).
 */

void lap_timer_init();
void lap_timer_set_track(int track_idx);
void lap_timer_poll();              // call after every gps_poll()
void lap_timer_reset_session();     // clear all laps, keep track
void lap_timer_set_ref_lap(uint8_t lap_idx);

// Internal: called by storage module after saving
void lap_timer_mark_saved(uint8_t lap_idx);
