#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * track_update — download the encrypted .tbrl bundle from the BRL server
 * and reload g_bundle_tracks.
 *
 * The file name is resolved from the active language (0=DE → Tracks_GER.tbrl,
 * 1=EN → Tracks_EN.tbrl). The customer-facing UI only calls
 * `track_update_run_blocking()`; the file names stay internal.
 *
 * Must be called on a task with WiFi STA connectivity, NOT from LVGL.
 * Blocks 10–30 s while downloading (~1 MB). Returns number of tracks
 * loaded on success, 0 or negative on failure.
 *
 * Flow:
 *   1. Derive URL: https://downloads.bavarian-racelabs.com/tracks/<file>
 *   2. HTTP GET into SPIRAM buffer, verify TBRL magic.
 *   3. Atomic replace: write to /sdcard/Tracks.tbrl.tmp then rename.
 *   4. Call tbrl_loader_load_default() → repopulates g_bundle_tracks.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -1 = network / HTTP error, -2 = no SD, -3 = bad payload, -4 = save failed.
   >0 = new bundle track count. */
int track_update_run_blocking(void);

#ifdef __cplusplus
}
#endif
