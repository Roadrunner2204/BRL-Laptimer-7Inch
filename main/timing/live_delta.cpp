/**
 * live_delta.cpp — Real-time lap delta computation
 */

#include "live_delta.h"
#include "../data/lap_data.h"
#include "../compat.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "live_delta";

// ---------------------------------------------------------------------------
// Search window: look +/-WINDOW points around cursor to handle GPS jitter
// ---------------------------------------------------------------------------
#define SEARCH_WINDOW  60   // +/-60 points = +/-6 s at 10 Hz

// EMA smoothing: alpha=0.3 → ~3-4 updates rolling average. Suppresses the
// ±100-200 ms jitter that comes from best_idx flipping between two adjacent
// ref points when the GPS position is on the boundary between them.
// SNAP threshold lets the smoother re-sync instantly on lap start or large
// genuine changes (braking zone entry, big mistake) instead of crawling.
#define SMOOTH_NUM_NEW   3
#define SMOOTH_NUM_OLD   7
#define SMOOTH_SNAP_MS   500

static const RecordedLap *s_ref = nullptr;
static int32_t            s_smooth_ms   = 0;
static bool               s_smooth_init = false;

// ---------------------------------------------------------------------------
// Haversine distance squared (approx, in degrees^2 — good enough for nearest)
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
    s_smooth_ms   = 0;
    s_smooth_init = false;
    log_i("Reference lap set (%u points)", ref_lap ? ref_lap->point_count : 0);
}

void live_delta_reset() {
    s_ref = nullptr;
    g_state.timing.ref_point_idx = 0;
    g_state.timing.live_delta_ms = 0;
    s_smooth_ms   = 0;
    s_smooth_init = false;
}

void live_delta_update(double lat, double lon, uint32_t elapsed_ms) {
    const RecordedLap *ref_lap = s_ref;
    if (!ref_lap || !ref_lap->valid || !ref_lap->points ||
        ref_lap->point_count == 0) return;

    // Freeze the delta once the driver has gone beyond the reference lap's
    // recorded length. Without this the cursor sticks at the last ref point
    // and (elapsed - last_point.lap_ms) just keeps growing linearly, which
    // looks exactly like a stopwatch ticking up on the delta display. Once
    // the next S/F crossing rewinds the cursor, the value snaps back —
    // producing the oscillation the user reported.
    uint32_t ref_total_ms = ref_lap->points[ref_lap->point_count - 1].lap_ms;
    if (ref_total_ms > 0 && elapsed_ms > ref_total_ms + 1000) {
        // Driver is more than 1 s past the end of the reference lap —
        // further comparison is meaningless. Keep the last computed delta
        // on screen until the new lap starts.
        return;
    }

    uint16_t &cursor = g_state.timing.ref_point_idx;
    if (cursor >= ref_lap->point_count) cursor = ref_lap->point_count - 1;

    // Search bidirectionally around cursor to handle GPS jitter
    int start = (int)cursor - (SEARCH_WINDOW / 4);  // look back ~1.5s
    if (start < 0) start = 0;
    int end = (int)cursor + SEARCH_WINDOW;
    if (end >= (int)ref_lap->point_count) end = (int)ref_lap->point_count - 1;

    double   best_d2  = 1e18;
    uint16_t best_idx = cursor;

    for (int i = start; i <= end; i++) {
        double d = dist2(lat, lon,
                         ref_lap->points[i].lat,
                         ref_lap->points[i].lon);
        if (d < best_d2) {
            best_d2  = d;
            best_idx = (uint16_t)i;
        }
    }

    cursor = best_idx;

    // ── Linear-interpolate ref_time along the polyline ────────────────
    //
    // Plain "ref_time = ref.lap_ms[best_idx]" is what made the old
    // delta tick like a stopwatch: when the driver moves only a few
    // metres between two GPS samples, best_idx stays the same → ref_time
    // is constant → delta = elapsed_ms - const just keeps growing. Then
    // a few seconds later best_idx jumps forward by N points, ref_time
    // catches up in one big step, and delta snaps back. Visually:
    // -0.2 → +0.8 over 1 s, snap to -0.4, +0.8 again, repeat.
    //
    // Fix: project the current GPS position onto the *polyline segment*
    // adjacent to best_idx, get a fractional position t ∈ [0, 1] along
    // it, and interpolate ref_time linearly between the segment's
    // endpoint timestamps. Now if you drive at exactly reference pace,
    // ref_time advances at exactly the same rate as elapsed_ms and the
    // delta stays flat — which is the whole point of a live-delta bar.
    auto project_t = [](double px, double py,
                        double ax, double ay,
                        double bx, double by) -> double {
        double vx = bx - ax, vy = by - ay;
        double wx = px - ax, wy = py - ay;
        double vv = vx * vx + vy * vy;
        if (vv < 1e-18) return 0.0;
        double t = (wx * vx + wy * vy) / vv;
        if (t < 0.0) return 0.0;
        if (t > 1.0) return 1.0;
        return t;
    };
    auto seg_dist2 = [project_t](double px, double py,
                                 double ax, double ay,
                                 double bx, double by, double *t_out) -> double {
        double t = project_t(px, py, ax, ay, bx, by);
        double cx = ax + t * (bx - ax);
        double cy = ay + t * (by - ay);
        double dx = px - cx, dy = py - cy;
        if (t_out) *t_out = t;
        return dx * dx + dy * dy;
    };

    // Work in the same equirectangular space as dist2() — scale lon by
    // cos(lat) so that distances are isotropic enough for projection.
    double cos_lat = cos(lat * M_PI / 180.0);
    double px = lat;
    double py = lon * cos_lat;

    int32_t ref_time_ms = ref_lap->points[best_idx].lap_ms;

    // Pick the better of the two adjacent segments [K-1,K] and [K,K+1]
    // (whichever is closer to the projected point). At the very first
    // and last point only one neighbour exists.
    int idx_a = -1, idx_b = -1;
    double t_pick = 0.0;

    auto try_segment = [&](int a, int b) {
        if (a < 0 || b >= (int)ref_lap->point_count || a == b) return;
        const TrackPoint &pa = ref_lap->points[a];
        const TrackPoint &pb = ref_lap->points[b];
        double ax = pa.lat, ay = pa.lon * cos_lat;
        double bx = pb.lat, by = pb.lon * cos_lat;
        double t = 0.0;
        double d2 = seg_dist2(px, py, ax, ay, bx, by, &t);
        if (idx_a < 0 || d2 < best_d2) {
            // Note: best_d2 was the nearest-point distance² above; using
            // it as a starting threshold keeps "stay on the closest
            // segment" the default tiebreaker.
            best_d2 = d2;
            idx_a = a; idx_b = b;
            t_pick = t;
        }
    };
    try_segment((int)best_idx - 1, (int)best_idx);
    try_segment((int)best_idx, (int)best_idx + 1);

    if (idx_a >= 0) {
        int32_t ms_a = (int32_t)ref_lap->points[idx_a].lap_ms;
        int32_t ms_b = (int32_t)ref_lap->points[idx_b].lap_ms;
        ref_time_ms = ms_a + (int32_t)(t_pick * (double)(ms_b - ms_a));
    }

    // delta = current time - interpolated ref time at same position
    int32_t raw = (int32_t)elapsed_ms - ref_time_ms;

    // Snap the smoother to raw on first sample after reset, on lap-start
    // (elapsed went backwards), or on a large genuine step change. Otherwise
    // EMA-blend to suppress nearest-point flicker.
    if (!s_smooth_init || abs(raw - s_smooth_ms) > SMOOTH_SNAP_MS) {
        s_smooth_ms   = raw;
        s_smooth_init = true;
    } else {
        s_smooth_ms = (s_smooth_ms * SMOOTH_NUM_OLD + raw * SMOOTH_NUM_NEW)
                    / (SMOOTH_NUM_OLD + SMOOTH_NUM_NEW);
    }
    g_state.timing.live_delta_ms = s_smooth_ms;
}
