/**
 * live_delta.cpp — Real-time lap delta computation
 */

#include "live_delta.h"
#include "../data/lap_data.h"
#include <math.h>
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Search window: look ±WINDOW points around cursor to handle GPS jitter
// ---------------------------------------------------------------------------
#define SEARCH_WINDOW  60   // ±60 points = ±6 s at 10 Hz

static const RecordedLap *s_ref = nullptr;

// ---------------------------------------------------------------------------
// Haversine distance squared (approx, in degrees² — good enough for nearest)
// We avoid sqrt for performance; only need relative distances.
// ---------------------------------------------------------------------------
static inline double dist2(double lat1, double lon1,
                            double lat2, double lon2) {
    double dlat = lat1 - lat2;
    double dlon = (lon1 - lon2) * cos(lat1 * M_PI / 180.0);
    return dlat * dlat + dlon * dlon;
}

void live_delta_set_ref(const RecordedLap *ref_lap) {
    s_ref = ref_lap;
    g_state.timing.ref_point_idx = 0;
    g_state.timing.live_delta_ms = 0;
}

void live_delta_reset() {
    s_ref = nullptr;
    g_state.timing.ref_point_idx = 0;
    g_state.timing.live_delta_ms = 0;
}

void live_delta_update(double lat, double lon, uint32_t elapsed_ms,
                       const RecordedLap *ref_lap) {
    if (!ref_lap || !ref_lap->valid || !ref_lap->points ||
        ref_lap->point_count == 0) return;

    uint16_t &cursor = g_state.timing.ref_point_idx;

    // Search forward from cursor within window
    int start = (int)cursor;
    int end   = start + SEARCH_WINDOW;
    if (end >= (int)ref_lap->point_count) end = (int)ref_lap->point_count - 1;

    double   best_d2  = 1e18;
    uint16_t best_idx = cursor;

    for (int i = start; i <= end; i++) {
        double d2 = dist2(lat, lon,
                          ref_lap->points[i].lat,
                          ref_lap->points[i].lon);
        if (d2 < best_d2) {
            best_d2  = d2;
            best_idx = (uint16_t)i;
        }
    }

    cursor = best_idx;

    // delta = current time − ref time at same position
    g_state.timing.live_delta_ms =
        (int32_t)elapsed_ms - (int32_t)ref_lap->points[best_idx].lap_ms;
}
