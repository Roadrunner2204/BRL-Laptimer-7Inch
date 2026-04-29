#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * obd_pid_cache — per-vehicle "which PIDs does this car answer?" memory.
 *
 * The BRL OBD BLE Adapter only speaks OBD2 Mode 01 (no Mode 09 → no
 * VIN read), so we can't fingerprint the car by VIN. Instead we key
 * the cache by the **active car profile name** (e.g. "BMW E92 M3.brl"
 * picked by the user in Settings → Vehicle profiles). That's at least
 * as specific as a VIN for our purposes — different M3 model years
 * with different PID support get different .brl files anyway.
 *
 * For CAN-direct vehicles the .brl is hand-curated for exactly one
 * engine, so the cache is somewhat redundant there — we still populate
 * it for consistency, but the dead-set will usually be empty.
 *
 * Storage: NVS namespace "obd_pidc", key = first 15 chars of car name
 * (NVS key length limit), blob = 256 bytes, one per PID:
 *   0 = unknown (never tried)
 *   1 = alive   (got STATUS_OK at least once for this car)
 *   2 = dead    (got TIMEOUT/NEGATIVE/etc. enough times)
 *
 * The cache survives reboots. Wipe it via obd_pid_cache_clear() when
 * the user explicitly wants a relearn (e.g. switched to a different
 * actual vehicle while keeping the same profile name).
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PID_UNKNOWN = 0,
    PID_ALIVE   = 1,
    PID_DEAD    = 2,
} PidCacheStatus;

/* Loads the cache for `car_profile_name`. Pass nullptr/empty to use a
 * generic "default" key (when no profile is active). Idempotent. */
void           obd_pid_cache_load(const char *car_profile_name);

PidCacheStatus obd_pid_cache_get(uint8_t pid);
void           obd_pid_cache_set(uint8_t pid, PidCacheStatus status);

/* Convenience — true only for confirmed-dead. Unknown stays alive-eligible. */
bool           obd_pid_cache_is_dead(uint8_t pid);

/* Persist to NVS if anything changed since last save. Cheap when clean. */
void           obd_pid_cache_save_if_dirty(void);

/* Wipe all PID statuses for the currently loaded car (Settings UI). */
void           obd_pid_cache_clear(void);

/* For diagnostics / settings UI. Returns count of (alive, dead). */
void           obd_pid_cache_counts(int *alive, int *dead);

#ifdef __cplusplus
}
#endif
