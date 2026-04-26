/**
 * recorder.c — orchestrates one recording session on the cam module.
 *
 * Owns the AVI writer + telemetry sidecar + cam-SD layout. Capture
 * input (MIPI-CSI → JPEG) is pushed in via recorder_push_jpeg_frame()
 * by the (still-TODO) camera capture pipeline; this layer is hardware-
 * independent and can be unit-tested end-to-end with synthetic frames.
 *
 * On-SD layout:
 *   /sdcard/sessions/<session_id>/video.avi      (this file)
 *   /sdcard/sessions/<session_id>/telemetry.brl  (TODO: sidecar)
 *   /sdcard/sessions/<session_id>/sync.json      (TODO: sidecar)
 *   /sdcard/sessions/<session_id>/meta.json      (this file, on start)
 */

#include "recorder.h"
#include "avi_writer.h"
#include "sd_mgr.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "recorder";

/* Capture profile. Match the OV5647 driver config in the future
 * camera bring-up — FHD at 30 fps is the OV5647 sweet spot per the
 * DFRobot tutorial. */
#define REC_WIDTH   1920
#define REC_HEIGHT  1080
#define REC_FPS     30

static bool                s_active = false;
static RecorderSessionInfo s_info   = {};
static char                s_session_dir[80] = {};
static char                s_video_path[160] = {};

/* ── Public API ─────────────────────────────────────────────────────── */

void recorder_init(void)
{
    avi_writer_preallocate();
    ESP_LOGI(TAG, "init done (avi_writer preallocated)");
}

void recorder_poll(void) { /* nothing periodic yet */ }

static void write_meta_json(void)
{
    char path[200];
    snprintf(path, sizeof(path), "%s/meta.json", s_session_dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "fopen(%s) failed", path);
        return;
    }
    fprintf(f,
        "{\n"
        "  \"session_id\":\"%s\",\n"
        "  \"gps_utc_ms\":%llu,\n"
        "  \"track_idx\":%ld,\n"
        "  \"track_name\":\"%s\",\n"
        "  \"car_name\":\"%s\",\n"
        "  \"video\":{\"w\":%d,\"h\":%d,\"fps\":%d,\"codec\":\"MJPG\"}\n"
        "}\n",
        s_info.session_id,
        (unsigned long long)s_info.gps_utc_ms,
        (long)s_info.track_idx,
        s_info.track_name,
        s_info.car_name,
        REC_WIDTH, REC_HEIGHT, REC_FPS);
    fclose(f);
}

bool recorder_start(const RecorderSessionInfo *info)
{
    if (!info) return false;
    if (s_active) recorder_stop();

    if (!sd_mgr_available()) {
        ESP_LOGE(TAG, "REC START failed: SD not available");
        return false;
    }

    s_info = *info;

    snprintf(s_session_dir, sizeof(s_session_dir),
             "/sdcard/sessions/%s", s_info.session_id);
    if (!sd_mgr_make_dirs(s_session_dir)) {
        ESP_LOGE(TAG, "make_dirs(%s) failed", s_session_dir);
        return false;
    }

    snprintf(s_video_path, sizeof(s_video_path), "%s/video.avi", s_session_dir);
    if (!avi_writer_open(s_video_path, REC_WIDTH, REC_HEIGHT, REC_FPS)) {
        ESP_LOGE(TAG, "avi_writer_open(%s) failed", s_video_path);
        return false;
    }

    write_meta_json();
    /* TODO: open telemetry.brl + sync.json once the sidecar writer
     * lands. For now the cam captures video only — telemetry frames
     * arriving via cam_link are just logged. */

    s_active = true;
    ESP_LOGI(TAG, "REC START session=%s track=%s car=%s utc=%llu",
             s_info.session_id, s_info.track_name, s_info.car_name,
             (unsigned long long)s_info.gps_utc_ms);
    return true;
}

bool recorder_stop(void)
{
    if (!s_active) return false;
    s_active = false;        /* set first so push_frame stops accepting */

    bool ok = avi_writer_close();
    ESP_LOGI(TAG, "REC STOP session=%s ok=%d", s_info.session_id, ok ? 1 : 0);
    /* TODO: close telemetry.brl + finalize sync.json. */
    return ok;
}

bool recorder_is_active(void) { return s_active; }

bool recorder_push_jpeg_frame(const uint8_t *jpeg, uint32_t size)
{
    if (!s_active) return false;
    return avi_writer_write_frame(jpeg, size);
}

void recorder_on_telemetry_gps(const CamTelemetryGps *t) {
    (void)t; /* TODO: sidecar */
}
void recorder_on_telemetry_obd(const CamTelemetryObd *t) {
    (void)t; /* TODO: sidecar */
}
void recorder_on_telemetry_analog(const CamTelemetryAnalog *t) {
    (void)t; /* TODO: sidecar */
}
void recorder_on_lap_marker(const CamLapMarker *m) {
    if (!s_active || !m) return;
    ESP_LOGI(TAG, "LAP %u: %lu ms (sectors=%u, video frame %lu)",
             m->lap_no, (unsigned long)m->lap_ms, m->sectors_used,
             (unsigned long)avi_writer_frame_count());
    /* TODO: append to sync.json so Studio can map video PTS ↔ lap_no. */
}

uint32_t recorder_get_session_bytes(void) { return avi_writer_file_size(); }
uint8_t  recorder_get_sd_free_pct(void)   { return sd_mgr_free_pct(); }
bool     recorder_has_sensor(void)        { return false; /* TODO: OV5647 detect */ }

const CamVideoListEntry *recorder_get_video_index(uint32_t *out_count)
{
    /* TODO: scan /sdcard/sessions/, populate a heap-allocated cache. */
    if (out_count) *out_count = 0;
    return NULL;
}
