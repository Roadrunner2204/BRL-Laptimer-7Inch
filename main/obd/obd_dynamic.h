#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * obd_dynamic — per-PID live value cache.
 *
 * Storage backing for dynamic dashboard slots (slot IDs 128..191): the
 * decoder writes the *post-scale/offset* value in here keyed by PID byte,
 * and the slot renderer reads it back when the user has assigned that
 * sensor to a Z3 slot.
 *
 * Replaces the old "fixed FIELD_RPM/THROTTLE/... struct fields" model
 * so any sensor from the Universal-OBD2-Liste (universal_obd_pids.h)
 * can be picked, not just the eight hard-coded slots.
 *
 * Per-PID freshness tracking lets the picker UI show a green "live"
 * badge on sensors that are currently producing data and grey out the
 * silent ones, without removing them from the picker entirely (so the
 * user can still assign a slot to a temporarily-quiet sensor).
 */

#ifdef __cplusplus
extern "C" {
#endif

#define OBD_DYNAMIC_PID_COUNT  256

// Default freshness window. A PID is considered "live" if it produced
// a value in the last LIVE_WINDOW_MS milliseconds. 4 s covers Multi-PID
// round-trip time (≈80 ms × 32 PIDs / 4 PIDs-per-bundle = ≈640 ms full
// cycle) with healthy slack for slow ECUs.
#define OBD_DYNAMIC_LIVE_WINDOW_MS  4000

// Decoder writes here every time a PID returns a value.
void  obd_dynamic_set(uint8_t pid, float value);

// Read latest value for a PID. Returns false when the PID has never
// produced a value this session (caller should render "--" then).
bool  obd_dynamic_get(uint8_t pid, float *out_value);

// Convenience: how old is the most recent value for this PID, in ms.
// Returns UINT32_MAX if nothing was ever written. Used by the picker
// to render the "live" badge.
uint32_t obd_dynamic_age_ms(uint8_t pid);

// True if a value arrived within the freshness window. Used to filter
// the picker list and to drive the live indicator.
bool  obd_dynamic_is_live(uint8_t pid);

// Wipe everything — called on disconnect so a fresh connect doesn't
// briefly show stale values from the previous session.
void  obd_dynamic_clear(void);

#ifdef __cplusplus
}
#endif
