#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
/// `lap_hint` selects the initial filename:
///   0  — manual recording, no session context: /sdcard/videos/REC_<ms>.avi
///   >0 — session is active, file is named <session_id>_lap<N>.avi
bool video_start_recording(uint8_t lap_hint);

/// Close current AVI and immediately open a new one named
/// <session_id>_lap<next_lap_no>.avi. No-op if:
///   - not currently recording
///   - current file is already named for this lap (e.g. A-B pre-roll matches)
/// Called from lap_timer on every start_lap() to give each lap its own video.
bool video_split_for_next_lap(uint8_t next_lap_no);

/// Basename (without .avi) of the currently open AVI, or "" if not recording.
/// Used by lap_timer to tag the just-finished lap in session JSON so the app
/// can map lap → video via GET /video/<stem>.
void video_get_current_filename_stem(char *buf, size_t len);

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

/// Get pointer to latest raw MJPEG preview frame.
/// Returns NULL if no preview available. size_out receives frame size.
const uint8_t *video_get_preview_mjpeg(uint32_t *size_out);

/// Get recording duration in seconds.
uint32_t video_get_rec_duration_s(void);

/// Get recording file size in bytes.
uint32_t video_get_rec_size_bytes(void);

/// Enable/disable raw-MJPEG passthrough recording mode.
///
/// When enabled (default: true), recording writes each incoming MJPEG frame
/// DIRECTLY to the AVI file without HW decode/overlay/encode. This removes
/// the entire video-pipeline CPU/DMA cost and lets the ESP sustain the full
/// camera frame rate (~30 fps at 720p/1080p). The data overlay is rendered
/// in the Android app at playback time.
///
/// When disabled, the legacy pipeline runs: MJPEG → HW decode → software
/// overlay → HW encode → AVI. Only use this for on-device overlay debugging.
void video_set_passthrough(bool enabled);
bool video_get_passthrough(void);

#ifdef __cplusplus
}
#endif
