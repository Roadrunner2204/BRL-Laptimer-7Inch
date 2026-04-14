#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * tbrl_loader — decrypt + parse /sdcard/Tracks.tbrl (AES-256-CBC).
 *
 * Fills global g_bundle_tracks / g_bundle_track_count (declared in
 * track_db.h). Allocates the track array in PSRAM. Safe to call even
 * if no bundle is present — in that case g_bundle_track_count stays 0.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Load bundle from /sdcard/Tracks.tbrl. Returns number of tracks loaded. */
int tbrl_loader_load_default(void);

/** Free bundle storage (rarely needed — only for re-import scenarios). */
void tbrl_loader_unload(void);

#ifdef __cplusplus
}
#endif
