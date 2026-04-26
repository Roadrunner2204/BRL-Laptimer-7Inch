#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SDMMC mount on /sdcard. Onboard slot of the DFR1172 — driver picks up
 * the BSP-defined pinout. Call sd_mgr_init() once at boot. */
void sd_mgr_init(void);
bool sd_mgr_available(void);

/* Free space helpers used by the STATUS heartbeat. */
uint64_t sd_mgr_total_bytes(void);
uint64_t sd_mgr_free_bytes(void);
uint8_t  sd_mgr_free_pct(void);

/* mkdir -p — creates /sdcard/<path> incl. all parent components.
 * Returns true if the leaf exists at the end, false on hard failure. */
bool sd_mgr_make_dirs(const char *path);

#ifdef __cplusplus
}
#endif
