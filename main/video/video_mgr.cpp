/**
 * video_mgr.cpp -- Video recording manager
 *
 * Coordinates UVC camera, JPEG pipeline, data overlay, and AVI writer.
 * Manages state transitions: IDLE → PREVIEW → RECORDING → IDLE.
 *
 * Auto-start: when timing becomes active and camera is connected,
 * recording starts automatically (if configured).
 */

#include "video_mgr.h"
#include "uvc_stream.h"
#include "video_pipeline.h"
#include "avi_writer.h"
#include "overlay.h"
#include "../data/lap_data.h"
#include "../storage/sd_mgr.h"
#include "../compat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "video_mgr";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static VideoState s_state = VIDEO_IDLE;
static bool s_pipeline_ready = false;
static uint32_t s_rec_start_ms = 0;
static bool s_auto_record = true;     // auto-start when timing begins
static bool s_was_timing_active = false;
static uint8_t s_quality = 80;

// Resolution management
static UvcResolution s_resolutions[UVC_MAX_RESOLUTIONS];
static int s_n_resolutions = 0;
static int s_res_index = -1;

// Task handle for the monitor task
static TaskHandle_t s_monitor_task = nullptr;

// ---------------------------------------------------------------------------
// UVC frame callback — called from USB context for each MJPEG frame
// ---------------------------------------------------------------------------
static void on_uvc_frame(const uint8_t *data, uint32_t size)
{
    if (s_state == VIDEO_RECORDING) {
        video_pipeline_process(data, size, true);
    } else if (s_state == VIDEO_PREVIEW) {
        video_pipeline_process(data, size, false);
    }
}

// ---------------------------------------------------------------------------
// Generate unique filename for recording
// ---------------------------------------------------------------------------
static void make_rec_path(char *buf, size_t len)
{
    // Use session ID if available, otherwise timestamp
    if (g_state.session.session_id[0]) {
        snprintf(buf, len, "/sdcard/videos/%s.avi", g_state.session.session_id);
    } else {
        uint32_t ms = millis();
        snprintf(buf, len, "/sdcard/videos/REC_%lu.avi", (unsigned long)ms);
    }
}

// ---------------------------------------------------------------------------
// Monitor task: watches for auto-record triggers
// ---------------------------------------------------------------------------
static void video_monitor_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));

        // Auto-start recording when timing becomes active
        if (s_auto_record && s_state == VIDEO_IDLE &&
            uvc_stream_connected() && sd_mgr_available()) {

            bool timing_now = g_state.timing.timing_active;
            if (timing_now && !s_was_timing_active) {
                log_i("Auto-start recording (timing active)");
                video_start_recording();
            }
            s_was_timing_active = timing_now;
        }

        // Auto-stop when timing ends
        if (s_state == VIDEO_RECORDING && s_auto_record) {
            if (!g_state.timing.timing_active && s_was_timing_active) {
                // Timing just stopped — keep recording for 3 more seconds
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (!g_state.timing.timing_active) {
                    log_i("Auto-stop recording (timing ended)");
                    video_stop_recording();
                }
            }
            s_was_timing_active = g_state.timing.timing_active;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void video_init(void)
{
    // Create /sdcard/videos directory
    sd_make_dir("/videos");

    // Initialize UVC camera subsystem
    uvc_stream_init();

    // Start monitor task
    xTaskCreatePinnedToCore(video_monitor_task, "vid_mon", 4096, nullptr, 2, &s_monitor_task, 0);

    log_i("Video manager initialized");
}

bool video_start_recording(void)
{
    if (s_state == VIDEO_RECORDING) {
        log_w("Already recording");
        return true;
    }

    if (!uvc_stream_connected()) {
        log_e("No camera connected");
        return false;
    }

    if (!sd_mgr_available()) {
        log_e("SD card not available");
        return false;
    }

    // Get resolution
    s_n_resolutions = uvc_stream_get_resolutions(s_resolutions, UVC_MAX_RESOLUTIONS);
    if (s_n_resolutions == 0) {
        log_e("No camera resolutions available");
        return false;
    }

    if (s_res_index < 0 || s_res_index >= s_n_resolutions) {
        s_res_index = s_n_resolutions - 1;  // default: highest
    }

    const UvcResolution &res = s_resolutions[s_res_index];

    // Initialize pipeline if needed
    if (!s_pipeline_ready) {
        if (!video_pipeline_init(res.width, res.height)) {
            log_e("Pipeline init failed");
            return false;
        }
        s_pipeline_ready = true;
    }
    video_pipeline_set_quality(s_quality);

    // Open AVI file
    char path[64];
    make_rec_path(path, sizeof(path));
    if (!avi_writer_open(path, res.width, res.height, res.fps)) {
        log_e("Cannot open AVI file");
        return false;
    }

    // Start UVC streaming
    uvc_stream_set_resolution(s_res_index);
    if (!uvc_stream_start(on_uvc_frame)) {
        log_e("UVC stream start failed");
        avi_writer_close();
        return false;
    }

    s_rec_start_ms = millis();
    s_state = VIDEO_RECORDING;
    g_state.video_recording = true;

    log_i("Recording started: %dx%d @ %d fps → %s",
          res.width, res.height, res.fps, path);
    return true;
}

void video_stop_recording(void)
{
    if (s_state != VIDEO_RECORDING) return;

    uvc_stream_stop();
    avi_writer_close();

    s_state = VIDEO_IDLE;
    g_state.video_recording = false;

    uint32_t dur = (millis() - s_rec_start_ms) / 1000;
    log_i("Recording stopped: %lu seconds, %lu frames",
          (unsigned long)dur, (unsigned long)avi_writer_frame_count());
}

void video_start_preview(void)
{
    if (s_state == VIDEO_RECORDING) return;  // don't interrupt recording
    if (!uvc_stream_connected()) return;

    s_n_resolutions = uvc_stream_get_resolutions(s_resolutions, UVC_MAX_RESOLUTIONS);
    if (s_n_resolutions == 0) return;

    if (s_res_index < 0 || s_res_index >= s_n_resolutions) {
        s_res_index = s_n_resolutions - 1;
    }

    const UvcResolution &res = s_resolutions[s_res_index];

    if (!s_pipeline_ready) {
        if (!video_pipeline_init(res.width, res.height)) return;
        s_pipeline_ready = true;
    }

    uvc_stream_set_resolution(s_res_index);
    uvc_stream_start(on_uvc_frame);
    s_state = VIDEO_PREVIEW;
    log_i("Preview started");
}

void video_stop_preview(void)
{
    if (s_state != VIDEO_PREVIEW) return;
    uvc_stream_stop();
    s_state = VIDEO_IDLE;
    log_i("Preview stopped");
}

VideoState video_get_state(void)
{
    return s_state;
}

bool video_camera_connected(void)
{
    return uvc_stream_connected();
}

int video_get_resolutions(VideoResolution *out, int max_count)
{
    s_n_resolutions = uvc_stream_get_resolutions(s_resolutions, UVC_MAX_RESOLUTIONS);
    int n = s_n_resolutions < max_count ? s_n_resolutions : max_count;
    for (int i = 0; i < n; i++) {
        out[i].width  = s_resolutions[i].width;
        out[i].height = s_resolutions[i].height;
        out[i].fps    = s_resolutions[i].fps;
    }
    return n;
}

void video_set_resolution(int index)
{
    s_res_index = index;
    uvc_stream_set_resolution(index);
}

int video_get_resolution_index(void)
{
    return s_res_index;
}

void video_set_quality(uint8_t quality)
{
    s_quality = quality;
    video_pipeline_set_quality(quality);
}

const uint8_t *video_get_preview_frame(uint16_t *width, uint16_t *height)
{
    return video_pipeline_get_preview(width, height);
}

uint32_t video_get_rec_duration_s(void)
{
    if (s_state != VIDEO_RECORDING) return 0;
    return (millis() - s_rec_start_ms) / 1000;
}

uint32_t video_get_rec_size_bytes(void)
{
    return avi_writer_file_size();
}
