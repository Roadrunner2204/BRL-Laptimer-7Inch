#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../data/lap_data.h"

/**
 * track_best — per-track all-time best lap persistence (NVS).
 *
 * Stores only the summary (total_ms + sector_ms[]) keyed by track name.
 * Track-points for live-delta are NOT stored here; live_delta continues
 * to reference the current session's fastest lap.
 *
 * UI purple mode: active while the current session has not yet produced
 * a lap faster than the stored best — driver is still chasing the record.
 */

typedef struct {
    uint32_t total_ms;
    uint32_t sector_ms[MAX_SECTORS];
    uint8_t  sectors_used;
    bool     valid;
} TrackBest;

#ifdef __cplusplus
extern "C" {
#endif

/** Load stored best for `track_name`. Sets valid=false if none. */
void track_best_load(const char *track_name, TrackBest *out);

/**
 * If `total_ms` beats stored (or no stored), save and return true.
 * Otherwise no-op and return false.
 */
bool track_best_maybe_update(const char *track_name,
                             uint32_t total_ms,
                             const uint32_t sector_ms[MAX_SECTORS],
                             uint8_t sectors_used);

/** Erase stored best for a track (debug / reset). */
void track_best_clear(const char *track_name);

#ifdef __cplusplus
}
#endif
