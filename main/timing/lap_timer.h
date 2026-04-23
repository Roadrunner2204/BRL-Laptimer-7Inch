#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../data/lap_data.h"
#include "../data/track_db.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 *   We test if segment (prev_pos -> curr_pos) intersects the timing line.
 *   This avoids false triggers from GPS jitter -- must actually cross.
 *
 * Minimum lap time guard: 20 seconds (prevents double-triggers at slow speed).
 */

void lap_timer_init(void);
void lap_timer_set_track(int track_idx);
void lap_timer_poll(void);              // call after every gps_poll()
void lap_timer_reset_session(void);     // clear all laps, keep track
void lap_timer_set_ref_lap(uint8_t lap_idx);

// Reference loaded from a SAVED session file (different from the live one).
// The lap's track_points are copied into an internal PSRAM buffer so the
// reference survives even if the source session file is later deleted.
// Returns false on failure (file missing, lap has no GPS points, etc.).
bool lap_timer_set_ref_from_saved(const char *session_id,
                                  uint8_t lap_idx_in_file);

// If an external (saved-session) reference is currently active, writes its
// session id into sid_out and its in-file lap index into lap_idx_out, and
// returns true. Returns false when the active reference is from the current
// session (or no reference is set).
bool lap_timer_get_external_ref(char *sid_out, size_t sid_size,
                                uint8_t *lap_idx_out);

// Internal: called by storage module after saving
void lap_timer_mark_saved(uint8_t lap_idx);

// Read-only access to the current in-progress lap GPS buffer.
// Returns NULL (and sets count_out=0) when not in a lap.
// Pointer is valid until the next S/F crossing.
const TrackPoint* lap_timer_get_cur_points(uint16_t *count_out);

#ifdef __cplusplus
}
#endif
