#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * capture — MIPI-CSI ⇒ ISP ⇒ HW-JPEG pipeline on the DFR1172.
 *
 * Owns one FreeRTOS task pinned to Core 1. While the recorder is
 * active, the task pulls frames from the OV5647 via esp_video (V4L2),
 * pushes each frame through the HW JPEG encoder, and forwards the
 * encoded buffer to recorder_push_jpeg_frame(). When the recorder is
 * idle the task sleeps without touching the camera, so we don't burn
 * power between sessions.
 *
 * Resolution / FPS are fixed at 1920×1080 / 30 to match the recorder's
 * AVI header. The OV5647 also supports 1280×720@60 if we ever need it.
 */

#define CAPTURE_WIDTH   1920
#define CAPTURE_HEIGHT  1080
#define CAPTURE_FPS     30

bool capture_init(void);                /* return false if sensor not detected */
bool capture_sensor_present(void);      /* mirrors recorder_has_sensor() */

/* Copy the most-recently-captured JPEG frame into the caller's buffer.
 * Used by the HTTP /preview.jpg handler so the laptimer can fetch a
 * fresh frame for the camera-aiming preview without disturbing the
 * recording pipeline.
 *
 * Returns the number of bytes written (0 if no frame available yet
 * or the buffer is too small). The buffer is owned by capture and
 * snapshotted under a short mutex — callers can free their copy
 * immediately. */
uint32_t capture_get_latest_jpeg(uint8_t *out, uint32_t max_size);

#ifdef __cplusplus
}
#endif
