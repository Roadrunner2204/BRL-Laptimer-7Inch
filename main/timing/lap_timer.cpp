/**
 * lap_timer.cpp — GPS-based automatic lap timing
 *
 * Line-crossing uses 2D segment intersection (cross-product sign change).
 * All timing via millis() — no RTC needed.
 * GPS track points stored in PSRAM (malloc'd once per lap).
 */

#include "lap_timer.h"
#include "live_delta.h"
#include "../storage/session_store.h"
#include "../compat.h"
#include <math.h>

static const char *TAG = "lap_timer";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define MIN_LAP_MS        20000   // 20 s guard — no double-trigger
#define MIN_SPEED_KMH     5.0f   // don't trigger when nearly stopped
#define POINT_INTERVAL_MS 100    // record GPS point every 100 ms (~10 Hz)

// ---------------------------------------------------------------------------
// Active track lines (pre-computed from TrackDef)
// ---------------------------------------------------------------------------
struct TimingLine {
    double x1, y1, x2, y2;   // approx flat-earth coords (meters)
    bool   active;
};

static TimingLine s_sf_line;
static TimingLine s_sector_lines[MAX_SECTORS];
static uint8_t    s_sector_count = 0;
static bool       s_is_circuit   = true;
static int        s_track_idx    = -1;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static double   s_prev_lat = 0, s_prev_lon = 0;
static bool     s_prev_valid = false;
static uint32_t s_last_point_ms = 0;

// PSRAM buffer for current lap
static TrackPoint *s_cur_buf   = nullptr;
static uint16_t    s_cur_count = 0;

// ---------------------------------------------------------------------------
// Helpers: flat-earth projection (good to ~10 km accuracy)
// ---------------------------------------------------------------------------
static const double DEG2M_LAT = 111320.0;

static inline double lat_to_m(double lat) { return lat * DEG2M_LAT; }
static inline double lon_to_m(double lon, double ref_lat) {
    return lon * DEG2M_LAT * cos(ref_lat * M_PI / 180.0);
}

// ---------------------------------------------------------------------------
// 2D cross product of vectors (p->q) and (p->r)
// ---------------------------------------------------------------------------
static inline double cross2d(double px, double py,
                              double qx, double qy,
                              double rx, double ry) {
    return (qx - px) * (ry - py) - (qy - py) * (rx - px);
}

// Check if segment (a->b) intersects segment (c->d)
static bool segments_intersect(double ax, double ay, double bx, double by,
                                double cx, double cy, double dx, double dy) {
    double d1 = cross2d(cx, cy, dx, dy, ax, ay);
    double d2 = cross2d(cx, cy, dx, dy, bx, by);
    double d3 = cross2d(ax, ay, bx, by, cx, cy);
    double d4 = cross2d(ax, ay, bx, by, dx, dy);

    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
        ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;

    return false;
}

// ---------------------------------------------------------------------------
// Build flat-earth TimingLine from two GPS points
// ---------------------------------------------------------------------------
static void build_line(TimingLine &tl,
                       double lat1, double lon1,
                       double lat2, double lon2) {
    double ref_lat = (lat1 + lat2) * 0.5;
    tl.x1 = lon_to_m(lon1, ref_lat);
    tl.y1 = lat_to_m(lat1);
    tl.x2 = lon_to_m(lon2, ref_lat);
    tl.y2 = lat_to_m(lat2);
    tl.active = (lat1 != 0.0 || lon1 != 0.0);
}

// ---------------------------------------------------------------------------
// Allocate PSRAM buffer for one lap
// ---------------------------------------------------------------------------
static void alloc_lap_buf() {
    if (s_cur_buf) free(s_cur_buf);
    s_cur_buf = (TrackPoint *)ps_malloc(sizeof(TrackPoint) * MAX_TRACK_POINTS);
    s_cur_count = 0;
    if (!s_cur_buf) {
        log_e("PSRAM alloc failed — track not recorded");
    }
}

// ---------------------------------------------------------------------------
// Finish the current in-progress lap and store it
// ---------------------------------------------------------------------------
static void finish_lap(uint32_t cross_ms) {
    LapSession &sess = g_state.session;
    LiveTiming &lt   = g_state.timing;

    if (!lt.in_lap) return;

    uint32_t lap_total_ms = cross_ms - lt.lap_start_ms;

    if (sess.lap_count >= MAX_LAPS_PER_SESSION) {
        log_w("Session full — lap discarded");
        return;
    }

    RecordedLap &rl = sess.laps[sess.lap_count];
    rl.total_ms     = lap_total_ms;
    rl.sectors_used = lt.current_sector;
    // Copy current sector time
    if (lt.current_sector < MAX_SECTORS) {
        rl.sector_ms[lt.current_sector] = cross_ms - lt.sector_start_ms;
    }
    rl.points      = s_cur_buf;
    rl.point_count = s_cur_count;
    rl.valid       = true;

    // Track best lap
    if (!sess.laps[sess.best_lap_idx].valid ||
        lap_total_ms < sess.laps[sess.best_lap_idx].total_ms) {
        sess.best_lap_idx = sess.lap_count;
        sess.ref_lap_idx  = sess.lap_count;
        live_delta_set_ref(&rl);
    }

    log_i("Lap %d: %lu ms (%u pts)",
          sess.lap_count + 1, (unsigned long)lap_total_ms, s_cur_count);

    sess.lap_count++;
    s_cur_buf   = nullptr;  // buffer now owned by RecordedLap
    s_cur_count = 0;

    // Async save (non-blocking stub — session_store queues it)
    session_store_save_lap(sess.lap_count - 1);
}

// ---------------------------------------------------------------------------
// Start a new lap
// ---------------------------------------------------------------------------
static void start_lap(uint32_t cross_ms) {
    LiveTiming &lt = g_state.timing;
    lt.lap_start_ms    = cross_ms;
    lt.sector_start_ms = cross_ms;
    lt.current_sector  = 0;
    lt.in_lap          = true;
    lt.timing_active   = true;
    lt.ref_point_idx   = 0;
    lt.live_delta_ms   = 0;
    lt.lap_number++;
    alloc_lap_buf();
    log_i("Start lap %d", lt.lap_number);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void lap_timer_init() {
    memset(&s_sf_line, 0, sizeof(s_sf_line));
    memset(s_sector_lines, 0, sizeof(s_sector_lines));
    s_prev_valid = false;
}

void lap_timer_set_track(int track_idx) {
    const TrackDef *td = track_get(track_idx);
    if (!td) return;

    s_track_idx  = track_idx;
    s_is_circuit = td->is_circuit;

    build_line(s_sf_line, td->sf_lat1, td->sf_lon1, td->sf_lat2, td->sf_lon2);

    s_sector_count = td->sector_count;
    for (uint8_t i = 0; i < td->sector_count && i < MAX_SECTORS; i++) {
        // Sector line: create a short perpendicular segment (+/-5 m) around point
        // Simplified: use +/-0.00005 deg offset as sector "gate width"
        build_line(s_sector_lines[i],
                   td->sectors[i].lat - 0.00005, td->sectors[i].lon,
                   td->sectors[i].lat + 0.00005, td->sectors[i].lon);
        s_sector_lines[i].active = true;
    }

    s_prev_valid = false;
    log_i("Track set: %s (%s), %u sectors",
          td->name, s_is_circuit ? "circuit" : "A-B", s_sector_count);
}

void lap_timer_reset_session() {
    LapSession &sess = g_state.session;
    // Free PSRAM buffers
    for (int i = 0; i < sess.lap_count; i++) {
        if (sess.laps[i].points) {
            free(sess.laps[i].points);
            sess.laps[i].points = nullptr;
        }
    }
    memset(&sess, 0, sizeof(sess));
    sess.track_idx    = s_track_idx;
    sess.best_lap_idx = 0;
    sess.ref_lap_idx  = 0;

    memset(&g_state.timing, 0, sizeof(g_state.timing));

    if (s_cur_buf) { free(s_cur_buf); s_cur_buf = nullptr; }
    s_cur_count  = 0;
    s_prev_valid = false;

    log_i("Session reset");
}

void lap_timer_set_ref_lap(uint8_t lap_idx) {
    LapSession &sess = g_state.session;
    if (lap_idx >= sess.lap_count) return;
    sess.ref_lap_idx = lap_idx;
    live_delta_set_ref(&sess.laps[lap_idx]);
    log_i("Reference lap set to %d", lap_idx + 1);
}

void lap_timer_poll() {
    if (s_track_idx < 0) return;
    if (!g_state.gps.valid) return;
    if (!s_sf_line.active) return;

    double cur_lat = g_state.gps.lat;
    double cur_lon = g_state.gps.lon;

    // Convert to flat-earth meters (referenced to SF midpoint)
    double ref_lat = (s_sf_line.y1 / DEG2M_LAT + s_sf_line.y2 / DEG2M_LAT) * 0.5;
    double cx = lon_to_m(cur_lon, ref_lat);
    double cy = lat_to_m(cur_lat);

    // Record GPS point at fixed interval
    uint32_t now_ms = millis();
    LiveTiming &lt  = g_state.timing;

    if (lt.in_lap && s_cur_buf && s_cur_count < MAX_TRACK_POINTS) {
        if (now_ms - s_last_point_ms >= POINT_INTERVAL_MS) {
            s_cur_buf[s_cur_count++] = { cur_lat, cur_lon,
                                         now_ms - lt.lap_start_ms };
            s_last_point_ms = now_ms;

            // Update live delta
            LapSession &sess = g_state.session;
            if (sess.laps[sess.ref_lap_idx].valid) {
                live_delta_update(cur_lat, cur_lon,
                                  now_ms - lt.lap_start_ms,
                                  &sess.laps[sess.ref_lap_idx]);
            }
        }
    }

    if (!s_prev_valid) {
        s_prev_lat   = cur_lat;
        s_prev_lon   = cur_lon;
        s_prev_valid = true;
        return;
    }

    double px = lon_to_m(s_prev_lon, ref_lat);
    double py = lat_to_m(s_prev_lat);

    // --- S/F line crossing ---
    if (g_state.gps.speed_kmh >= MIN_SPEED_KMH &&
        segments_intersect(px, py, cx, cy,
                           s_sf_line.x1, s_sf_line.y1,
                           s_sf_line.x2, s_sf_line.y2)) {

        uint32_t guard = now_ms - lt.lap_start_ms;
        if (!lt.in_lap || guard >= MIN_LAP_MS) {
            if (lt.in_lap) finish_lap(now_ms);
            start_lap(now_ms);
        }
    }

    // --- Sector line crossings ---
    if (lt.in_lap) {
        for (uint8_t i = lt.current_sector; i < s_sector_count; i++) {
            if (!s_sector_lines[i].active) continue;
            if (segments_intersect(px, py, cx, cy,
                                   s_sector_lines[i].x1, s_sector_lines[i].y1,
                                   s_sector_lines[i].x2, s_sector_lines[i].y2)) {
                uint32_t sector_ms = now_ms - lt.sector_start_ms;
                g_state.session.laps[g_state.session.lap_count].sector_ms[lt.current_sector]
                    = sector_ms;
                log_i("Sector %d: %lu ms",
                      lt.current_sector + 1, (unsigned long)sector_ms);
                lt.sector_start_ms = now_ms;
                lt.current_sector++;
                break;  // only one sector per poll
            }
        }
    }

    s_prev_lat = cur_lat;
    s_prev_lon = cur_lon;
}

void lap_timer_mark_saved(uint8_t /*lap_idx*/) {
    // placeholder — could set a "saved" flag on RecordedLap
}

const TrackPoint* lap_timer_get_cur_points(uint16_t *count_out) {
    if (count_out) *count_out = g_state.timing.in_lap ? s_cur_count : 0;
    return (g_state.timing.in_lap && s_cur_buf) ? s_cur_buf : nullptr;
}
