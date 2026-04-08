#pragma once
#include <stdint.h>
#include "../data/lap_data.h"

/**
 * live_delta — Real-time delta vs reference lap at every GPS position
 *
 * Algorithm:
 *   1. Each lap's GPS track is an array of TrackPoint {lat, lon, lap_ms}.
 *   2. When a new GPS position arrives, find the closest point in the
 *      reference lap track (starting from the last cursor position --
 *      monotonic forward search in a limited window to prevent jumps).
 *   3. delta_ms = current_elapsed_ms - reference_point.lap_ms
 *      Positive = slower than reference.  Negative = faster.
 *   4. Result stored in g_state.timing.live_delta_ms.
 */

void live_delta_set_ref(const RecordedLap *ref_lap);
void live_delta_update(double lat, double lon, uint32_t elapsed_ms,
                       const RecordedLap *ref_lap);
void live_delta_reset();
