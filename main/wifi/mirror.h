/**
 * mirror.h -- Screen-mirror module for the Android remote-control app.
 *
 * Captures the live LVGL screen, encodes it to JPEG via the ESP32-P4
 * hardware encoder, and exposes a virtual touch input device so taps
 * forwarded from the phone are injected into LVGL exactly as if the
 * physical GT911 touchscreen had been pressed.
 *
 * The phone receives an MJPEG stream over /mirror.mjpeg (one client at
 * a time -- the HW JPEG encoder is single-instance) and POSTs touch
 * events to /touch in display logical coordinates.
 *
 * Allocation policy: lazy. The first acquire allocates the JPEG encoder
 * + ~1.5 MB of DMA-capable PSRAM for input/output buffers. Memory stays
 * allocated until reboot so subsequent connections re-attach without
 * re-init lag.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise the virtual touch input device. Call once after lv_my_setup(). */
void mirror_init(void);

/**
 * Reserve the mirror stream slot.
 *
 * Returns false if another client is already streaming -- there is only
 * one HW JPEG encoder, so concurrent streams would race. Caller must
 * pair every successful acquire with mirror_release_stream().
 */
bool mirror_acquire_stream(void);
void mirror_release_stream(void);

/**
 * Capture the active LVGL screen and encode it to JPEG.
 *
 * On ESP_OK, *out_buf points to module-owned memory holding the JPEG
 * bytes and *out_size is the byte count. The pointer remains valid only
 * until the NEXT call to mirror_capture_jpeg() (the buffer is reused).
 *
 * Caller must currently hold the stream slot.
 */
esp_err_t mirror_capture_jpeg(uint8_t **out_buf, size_t *out_size);

/** Inject a touch event in LVGL display logical coordinates (0..1023, 0..599). */
void mirror_inject_touch(int16_t x, int16_t y, bool pressed);

/**
 * Append UTF-8 text to the textarea bound to the currently visible LVGL
 * keyboard widget (the laptimer always pairs lv_textarea + lv_keyboard,
 * so the visible keyboard's bound textarea is the user's intended target).
 *
 * No-op if no keyboard is on screen. Returns true on hit.
 */
bool mirror_inject_text(const char *utf8);

/** Delete one character from the focused textarea (one keystroke). */
bool mirror_inject_backspace(void);

#ifdef __cplusplus
}
#endif
