#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * uvc_stream -- USB Video Class camera abstraction
 *
 * Wraps the ESP-IDF USB Host UVC driver for streaming MJPEG frames
 * from a USB camera. Supports resolution enumeration and selection.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define UVC_MAX_RESOLUTIONS  8

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
    uint8_t  format_idx;   // internal UVC format index
    uint8_t  frame_idx;    // internal UVC frame index
} UvcResolution;

/// Callback invoked when a complete MJPEG frame is received.
/// @param data   MJPEG frame data (valid only during callback)
/// @param size   frame size in bytes
typedef void (*uvc_frame_cb_t)(const uint8_t *data, uint32_t size);

/// Initialize USB Host and UVC driver. Call once.
void uvc_stream_init(void);

/// Returns true if a UVC camera is currently connected.
bool uvc_stream_connected(void);

/// Get available resolutions from the connected camera.
/// Returns count (0 if no camera).
int uvc_stream_get_resolutions(UvcResolution *out, int max_count);

/// Select a resolution by index (from get_resolutions). Default: highest.
void uvc_stream_set_resolution(int index);

/// Start streaming. Frames are delivered via the callback.
bool uvc_stream_start(uvc_frame_cb_t cb);

/// Stop streaming.
void uvc_stream_stop(void);

/// Get the active resolution.
void uvc_stream_get_active_resolution(uint16_t *w, uint16_t *h, uint8_t *fps);

#ifdef __cplusplus
}
#endif
