#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cam_preview — fetches the cam module's /preview.jpg over HTTP,
 * HW-decodes the JPEG, and paints it onto an LVGL canvas. Used by the
 * Video Settings screen so the user can aim the camera in real time.
 *
 * Background worker runs at ~5 Hz so the preview reacts quickly enough
 * for aiming without saturating the WiFi link or LVGL render thread.
 *
 * The worker pulls the cam's IP from cam_link_get_info() — same path
 * the data_server uses for its 302 redirect — so there's no separate
 * pairing step.
 */

/* Start the preview pump. `canvas` must be an lv_canvas with an
 * already-attached RGB565 buffer of (w * h * 2) bytes (use
 * lv_canvas_set_buffer before calling this). The pointer is captured
 * and used until cam_preview_close(). */
void cam_preview_open(lv_obj_t *canvas, uint16_t w, uint16_t h);

/* Stop the worker, free the JPEG decoder, release HTTP client. Safe to
 * call when the preview was never opened. */
void cam_preview_close(void);

/* True if the most recent fetch + decode succeeded within the last 2 s.
 * Drives the "No signal" overlay on the Video Settings screen. */
bool cam_preview_has_signal(void);

#ifdef __cplusplus
}
#endif
