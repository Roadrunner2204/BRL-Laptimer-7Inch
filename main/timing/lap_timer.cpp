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
#include "../storage/track_best.h"
#include "../data/track_db.h"
#include "../compat.h"
#include <math.h>

static const char *TAG = "lap_timer";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define MIN_LAP_MS        20000   // 20 s guard — circuit double-trigger protection
#define MIN_AB_FIN_MS     2000    // 2 s guard for A-B finish (short drag runs possible)
#define MIN_SPEED_KMH     5.0f   // don't trigger when nearly stopped
#define POINT_INTERVAL_MS 100    // record GPS point every 100 ms (~10 Hz)
#define SECTOR_GATE_M     15.0   // half-width of sector gate in meters

// ---------------------------------------------------------------------------
// Active track lines (pre-computed from TrackDef)
// ---------------------------------------------------------------------------
struct TimingLine {
    double x1, y1, x2, y2;   // approx flat-earth coords (meters)
    bool   active;
};

// Sector gates support two geometries (picked per-sector at track load):
//   is_line=false → X-shape (two crossed diagonals, from single-point data).
//                   Triggers on any car-direction crossing.
//   is_line=true  → one straight segment from (x1,y1) to (x2,y2) in line_a,
//                   line_b unused. Used when the source data already carries
//                   a perpendicular 2-point line (VBOX .tbrl tracks).
struct SectorGate {
    TimingLine line_a;   // X-shape NE-SW diagonal  OR  straight 2-point line
    TimingLine line_b;   // X-shape NW-SE diagonal  (ignored when is_line=true)
    bool       active;
    bool       is_line;  // true → use only line_a; false → X-shape (a+b)
};

static TimingLine s_sf_line;
static TimingLine s_fin_line;     // separate finish for A-B tracks
static SectorGate s_sector_gates[MAX_SECTORS];
static uint8_t    s_sector_count = 0;
static bool       s_is_circuit   = true;
static int        s_track_idx    = -1;
// Single track-wide reference latitude. ALL coordinates in the timing
// system (SF line, FIN line, sector gates, current car position) are
// projected using this one value, so segments_intersect() math is consistent.
static double     s_ref_lat      = 0.0;

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
// Build flat-earth TimingLine from two GPS points, using the track-wide
// s_ref_lat so every line in the session shares one coordinate frame.
// Without this, each line had its own cos(lat) scaling and cross-checks
// against the car position (projected with SF's ref_lat) silently failed.
// ---------------------------------------------------------------------------
static void build_line(TimingLine &tl,
                       double lat1, double lon1,
                       double lat2, double lon2) {
    tl.x1 = lon_to_m(lon1, s_ref_lat);
    tl.y1 = lat_to_m(lat1);
    tl.x2 = lon_to_m(lon2, s_ref_lat);
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
    // Record the final (in-progress) sector time at S/F crossing
    if (lt.current_sector < MAX_SECTORS) {
        rl.sector_ms[lt.current_sector] = cross_ms - lt.sector_start_ms;
        rl.sectors_used = lt.current_sector + 1;  // include the final sector
    } else {
        rl.sectors_used = lt.current_sector;
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

    // Persist all-time best per track (NVS, summary only — no points).
    // Skipped for A-B stages since total_ms there isn't a circuit lap time.
    extern bool g_chasing_record;
    if (s_is_circuit && s_track_idx >= 0) {
        const TrackDef *td = track_get(s_track_idx);
        if (td) {
            bool new_record = track_best_maybe_update(td->name, lap_total_ms,
                                    rl.sector_ms, rl.sectors_used);
            if (new_record) {
                // Session has beaten the stored record — leave "chasing"
                // state: switch sector/delta colors from purple to green/red.
                g_chasing_record = false;
                log_i("Record broken for '%s' — purple mode off", td->name);
            }
        }
    }

    sess.lap_count++;
    s_cur_buf   = nullptr;  // buffer now owned by RecordedLap
    s_cur_count = 0;

    // CRITICAL: mark lap as finished. For circuits, start_lap() is called
    // right after and sets this back to true. For A-B, it stays false until
    // the user crosses the start line again for a new run.
    lt.in_lap = false;

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
    memset(s_sector_gates, 0, sizeof(s_sector_gates));
    s_prev_valid = false;
}

void lap_timer_set_track(int track_idx) {
    const TrackDef *td = track_get(track_idx);
    if (!td) return;

    s_track_idx  = track_idx;
    s_is_circuit = td->is_circuit;

    // Pick ONE reference latitude for the whole track. Using the SF midpoint
    // gives good accuracy across A-B tracks up to ~10 km long. All
    // subsequent build_line() calls + the poll loop use this same value.
    s_ref_lat = (td->sf_lat1 + td->sf_lat2) * 0.5;
    if (s_ref_lat == 0.0) s_ref_lat = td->sf_lat1;  // fallback

    build_line(s_sf_line, td->sf_lat1, td->sf_lon1, td->sf_lat2, td->sf_lon2);

    // For A-B tracks: build separate finish line
    if (!s_is_circuit) {
        build_line(s_fin_line, td->fin_lat1, td->fin_lon1, td->fin_lat2, td->fin_lon2);
        if (!s_fin_line.active) {
            log_e("A-B TRACK WITHOUT FINISH LINE: '%s' has no fin_lat1/lon1 set! "
                  "Timer will never stop — please edit the track and set the "
                  "finish-line coordinates.", td->name);
        }
    } else {
        s_fin_line.active = false;
    }

    // Cap at MAX_SECTORS — otherwise the poll loop would index past
    // s_sector_gates[] and crash. Track data might claim more sectors
    // than we have storage for.
    s_sector_count = td->sector_count;
    if (s_sector_count > MAX_SECTORS) s_sector_count = MAX_SECTORS;
    for (uint8_t i = 0; i < s_sector_count; i++) {
        const SectorLine &sl = td->sectors[i];
        bool have_p2 = (sl.lat2 != 0.0 || sl.lon2 != 0.0);

        if (have_p2) {
            // 2-point straight line (from VBOX .tbrl data, or any future
            // source that already provides a precise perpendicular segment).
            build_line(s_sector_gates[i].line_a,
                       sl.lat,  sl.lon,
                       sl.lat2, sl.lon2);
            s_sector_gates[i].line_b.active = false;
            s_sector_gates[i].is_line = true;
        } else {
            // X-shaped gate: two crossed lines (~30m across each) centered
            // on the single sector point. Catches crossings at any track
            // direction because with two perpendicular diagonals at least
            // one is never parallel to the car's travel direction.
            double lat_off = SECTOR_GATE_M / DEG2M_LAT;   // ~15 m in latitude
            double lon_off = SECTOR_GATE_M /
                             (DEG2M_LAT * cos(sl.lat * M_PI / 180.0));
            double slat = sl.lat;
            double slon = sl.lon;

            build_line(s_sector_gates[i].line_a,
                       slat - lat_off, slon - lon_off,
                       slat + lat_off, slon + lon_off);
            build_line(s_sector_gates[i].line_b,
                       slat + lat_off, slon - lon_off,
                       slat - lat_off, slon + lon_off);
            s_sector_gates[i].is_line = false;
        }
        s_sector_gates[i].active = true;
    }

    s_prev_valid = false;

    // Enter purple "chasing" mode iff a stored all-time best exists for
    // this track. As soon as the current session produces a new record
    // (finish_lap below), the flag clears and colors go back to green/red.
    extern bool g_chasing_record;
    TrackBest tb;
    track_best_load(td->name, &tb);
    g_chasing_record = (s_is_circuit && tb.valid);

    log_i("Track set: %s (%s), %u sectors",
          td->name, s_is_circuit ? "circuit" : "A-B", s_sector_count);
    log_i("  SF line : (%.6f, %.6f) -> (%.6f, %.6f)",
          td->sf_lat1, td->sf_lon1, td->sf_lat2, td->sf_lon2);
    if (!s_is_circuit) {
        log_i("  FIN line: (%.6f, %.6f) -> (%.6f, %.6f)  active=%d",
              td->fin_lat1, td->fin_lon1, td->fin_lat2, td->fin_lon2,
              s_fin_line.active ? 1 : 0);
        // Report SF↔FIN distance in meters so user can sanity-check
        double dx = s_fin_line.x1 - s_sf_line.x1;
        double dy = s_fin_line.y1 - s_sf_line.y1;
        log_i("  SF-to-FIN distance: %.1f m  (ref_lat=%.6f)",
              sqrt(dx*dx + dy*dy), s_ref_lat);
    }
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

    // Project current position using the track-wide reference latitude,
    // so all line-crossing math shares one consistent coordinate frame.
    double cx = lon_to_m(cur_lon, s_ref_lat);
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

    double px = lon_to_m(s_prev_lon, s_ref_lat);
    double py = lat_to_m(s_prev_lat);

    // --- S/F line crossing ---
    if (g_state.gps.speed_kmh >= MIN_SPEED_KMH) {

        bool cross_sf = segments_intersect(px, py, cx, cy,
                            s_sf_line.x1, s_sf_line.y1,
                            s_sf_line.x2, s_sf_line.y2);

        if (s_is_circuit) {
            // Circuit: S/F line is both start and finish
            if (cross_sf) {
                uint32_t guard = now_ms - lt.lap_start_ms;
                if (!lt.in_lap || guard >= MIN_LAP_MS) {
                    log_i("S/F crossed (circuit)");
                    if (lt.in_lap) finish_lap(now_ms);
                    start_lap(now_ms);
                }
            }
        } else {
            // ── A-B track logic ──────────────────────────────────
            // Start line: begins a new run only if NOT currently running.
            //   Crossing the start line again during a run is ignored
            //   (driver is never "finishing" at the start on an A-B track).
            // Finish line: ends current run, keeps times shown for review.
            //   New run only begins on next start-line crossing.
            if (cross_sf && !lt.in_lap) {
                log_i("Start line crossed — begin A-B run");
                start_lap(now_ms);
            }
            if (lt.in_lap && s_fin_line.active) {
                bool cross_fin = segments_intersect(px, py, cx, cy,
                                     s_fin_line.x1, s_fin_line.y1,
                                     s_fin_line.x2, s_fin_line.y2);
                uint32_t guard = now_ms - lt.lap_start_ms;

                // Proximity diagnostic: if car is within 30 m of the finish
                // line midpoint, log the segment intersection state once per
                // ~500 ms so we can see WHY a crossing isn't triggering.
                static uint32_t s_last_fin_diag_ms = 0;
                double fmx = (s_fin_line.x1 + s_fin_line.x2) * 0.5;
                double fmy = (s_fin_line.y1 + s_fin_line.y2) * 0.5;
                double dx = cx - fmx, dy = cy - fmy;
                double dist_m = sqrt(dx*dx + dy*dy);
                if (dist_m < 30.0 && (now_ms - s_last_fin_diag_ms) > 500) {
                    log_i("FIN proximity: %.1f m, cross=%d, guard=%lums",
                          dist_m, cross_fin ? 1 : 0, (unsigned long)guard);
                    s_last_fin_diag_ms = now_ms;
                }

                if (cross_fin && guard >= MIN_AB_FIN_MS) {
                    log_i("Finish line crossed — A-B run done");
                    finish_lap(now_ms);
                    // finish_lap sets in_lap=false; timing_active stays true
                    // so the UI keeps showing the last time until next start.
                }
            }
        }
    }

    // --- Sector gate crossings (X-shape: trigger on either line) ---
    if (lt.in_lap &&
        lt.current_sector < MAX_SECTORS &&
        g_state.session.lap_count < MAX_LAPS_PER_SESSION)
    {
        uint8_t end = s_sector_count;
        if (end > MAX_SECTORS) end = MAX_SECTORS;
        for (uint8_t i = lt.current_sector; i < end; i++) {
            if (!s_sector_gates[i].active) continue;
            const TimingLine &la = s_sector_gates[i].line_a;
            bool cross_a = segments_intersect(px, py, cx, cy,
                                              la.x1, la.y1, la.x2, la.y2);
            bool cross_b = false;
            if (!s_sector_gates[i].is_line) {
                const TimingLine &lb = s_sector_gates[i].line_b;
                cross_b = segments_intersect(px, py, cx, cy,
                                             lb.x1, lb.y1, lb.x2, lb.y2);
            }
            if (cross_a || cross_b) {
                uint32_t sector_ms = now_ms - lt.sector_start_ms;
                g_state.session.laps[g_state.session.lap_count].sector_ms[lt.current_sector]
                    = sector_ms;
                log_i("Sector %d: %lu ms", lt.current_sector + 1, (unsigned long)sector_ms);
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

