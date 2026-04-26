/**
 * recorder.c — STUB. Wires the cam_link inputs to a (TODO) MIPI-CSI →
 * JPEG-encode → AVI-write pipeline + telemetry sidecar.
 *
 * Production work that is intentionally NOT in this skeleton — each of
 * these is its own follow-up:
 *
 *   1. MIPI-CSI bring-up with esp_video on the OV5647 (1080p30 RAW10).
 *      Reference: examples/peripherals/isp/multi_pipelines.
 *   2. HW JPEG encoder driver (esp_driver_jpeg).
 *   3. AVI writer — recycle from main repo's deleted commit e08cb92
 *      (main/video/avi_writer.{h,cpp} survives in git history).
 *   4. Telemetry sidecar writer in the laptimer's existing .brl format
 *      so Studio uses the same parser for laptimer-SD and cam-SD imports.
 *   5. Atomic file rename on stop so partial writes are easy to detect.
 *
 * For now the stub just logs activity so the laptimer-side integration
 * can be brought up end-to-end before the heavy capture code lands.
 */

#include "recorder.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "recorder";

static bool                s_active = false;
static RecorderSessionInfo s_info   = {};
static uint32_t            s_bytes  = 0;

void recorder_init(void) {
    ESP_LOGI(TAG, "init (stub)");
}

void recorder_poll(void) {
    /* TODO: drain camera frame queue, write to AVI, update s_bytes. */
}

bool recorder_start(const RecorderSessionInfo *info) {
    if (!info) return false;
    if (s_active) recorder_stop();
    s_info = *info;
    s_bytes = 0;
    s_active = true;
    ESP_LOGI(TAG, "REC START session=%s track=%s car=%s utc=%llu",
             s_info.session_id, s_info.track_name, s_info.car_name,
             (unsigned long long)s_info.gps_utc_ms);
    /* TODO: mkdir /sdcard/sessions/<id>/, open video.avi + telemetry.brl,
     * write meta.json + sync.json header. */
    return true;
}

bool recorder_stop(void) {
    if (!s_active) return false;
    ESP_LOGI(TAG, "REC STOP session=%s bytes=%lu",
             s_info.session_id, (unsigned long)s_bytes);
    s_active = false;
    /* TODO: close all file handles, write final sync.json, fsync SD. */
    return true;
}

bool recorder_is_active(void) { return s_active; }

void recorder_on_telemetry_gps(const CamTelemetryGps *t) {
    if (!s_active || !t) return;
    /* TODO: append to telemetry.brl in laptimer-compatible format. */
    ESP_LOGV(TAG, "GPS %.5f,%.5f spd=%.1f", t->lat, t->lon, t->speed_kmh);
}
void recorder_on_telemetry_obd(const CamTelemetryObd *t) {
    if (!s_active || !t) return;
    ESP_LOGV(TAG, "OBD rpm=%.0f tps=%.1f", t->rpm, t->throttle_pct);
}
void recorder_on_telemetry_analog(const CamTelemetryAnalog *t) {
    if (!s_active || !t) return;
    ESP_LOGV(TAG, "ANA mask=0x%02X", t->valid_mask);
}
void recorder_on_lap_marker(const CamLapMarker *m) {
    if (!s_active || !m) return;
    ESP_LOGI(TAG, "LAP %u: %lu ms (sectors=%u)",
             m->lap_no, (unsigned long)m->lap_ms, m->sectors_used);
    /* TODO: write into sync.json so Studio can map video_pts ↔ lap_no. */
}

uint32_t recorder_get_session_bytes(void) { return s_bytes; }

uint8_t recorder_get_sd_free_pct(void) {
    /* TODO: statvfs("/sdcard") and compute free pct. */
    return 100;
}

bool recorder_has_sensor(void) {
    /* TODO: report the OV5647 detect status from esp_video init. */
    return false;
}

const CamVideoListEntry *recorder_get_video_index(uint32_t *out_count) {
    /* TODO: scan /sdcard/sessions/, populate a heap-allocated cache. */
    if (out_count) *out_count = 0;
    return NULL;
}
