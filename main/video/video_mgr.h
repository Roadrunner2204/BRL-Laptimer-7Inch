#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * video_mgr -- Video recording manager
 *
 * Coordinates USB UVC camera, JPEG pipeline, data overlay, and AVI writer.
 * All video tasks run on Core 0 alongside the logic task.
 *
 * States:
 *   IDLE       -- no camera connected or not initialized
 *   PREVIEW    -- camera streaming, frames decoded for settings preview
 *   RECORDING  -- full pipeline: decode → overlay → encode → AVI on SD
 *   ERROR      -- camera disconnected or SD error during recording
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIDEO_IDLE = 0,
    VIDEO_PREVIEW,
    VIDEO_RECORDING,
    VIDEO_ERROR,
} VideoState;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
} VideoResolution;

/// Initialize video subsystem (USB host, tasks). Call once from app_main.
void video_init(void);

/// Start recording to SD card. Returns true on success.
bool video_start_recording(void);

/// Stop recording and finalize AVI file.
void video_stop_recording(void);

/// Start camera preview (for settings screen).
void video_start_preview(void);

/// Stop camera preview.
void video_stop_preview(void);

/// Get current video state.
VideoState video_get_state(void);

/// Returns true if a USB camera is connected and enumerated.
bool video_camera_connected(void);

/// Get available resolutions. Returns count, fills array up to max_count.
int video_get_resolutions(VideoResolution *out, int max_count);

/// Set desired resolution. Takes effect on next recording/preview start.
void video_set_resolution(int index);

/// Get current resolution index.
int video_get_resolution_index(void);

/// Set JPEG quality (30-95). Default 80.
void video_set_quality(uint8_t quality);

/// Get pointer to latest decoded RGB565 preview frame (for LVGL display).
/// Returns NULL if no preview available. Caller must NOT free.
/// width/height are output parameters.
const uint8_t *video_get_preview_frame(uint16_t *width, uint16_t *height);

/// Get recording duration in seconds.
uint32_t video_get_rec_duration_s(void);

/// Get recording file size in bytes.
uint32_t video_get_rec_size_bytes(void);

#ifdef __cplusplus
}
#endif
