#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * obd_status — per-FieldId "freshness" tracker.
 *
 * Whenever `route_sensor()` (in obd_bt.cpp) successfully routes a value
 * into a g_state.obd.* slot, the originating FieldId is stamped here
 * with millis().
 *
 * The Display-, App- and Studio-side slot pickers consult this tracker
 * to grey out fields the car/adapter never actually delivers, so the
 * user only sees what's really live. A field is "live" when its last
 * stamp is within OBD_STATUS_LIVE_WINDOW_MS (default 5 s).
 *
 * Wire format for the HTTP `/obd_status` endpoint:
 *   { "now": 12345678, "fields": { "32": 12345600, "33": 12340000, ... } }
 *
 * Studio + App fetch this and compare `now - last < 5000`.
 */

#define OBD_STATUS_LIVE_WINDOW_MS 5000

#ifdef __cplusplus
extern "C" {
#endif

void     obd_status_mark_active(uint8_t field_id);
bool     obd_status_is_live(uint8_t field_id);
uint32_t obd_status_last_ms(uint8_t field_id);

/* Renders a JSON snapshot of the active fields. Returns the number of
 * bytes written (excluding the terminating NUL); 0 on truncation/error. */
size_t   obd_status_render_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
