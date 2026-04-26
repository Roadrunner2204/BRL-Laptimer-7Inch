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
#include "sidecar.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

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
    sidecar_open(s_session_dir, s_info.session_id, s_info.gps_utc_ms,
                 s_info.track_idx, s_info.track_name, s_info.car_name);

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

    uint32_t frames = avi_writer_frame_count();
    bool ok = avi_writer_close();
    sidecar_close(frames);
    s_idx_dirty = true;     /* new video.avi on disk — refresh index next read */
    ESP_LOGI(TAG, "REC STOP session=%s ok=%d frames=%lu",
             s_info.session_id, ok ? 1 : 0, (unsigned long)frames);
    return ok;
}

bool recorder_is_active(void) { return s_active; }

bool recorder_push_jpeg_frame(const uint8_t *jpeg, uint32_t size)
{
    if (!s_active) return false;
    return avi_writer_write_frame(jpeg, size);
}

void recorder_on_telemetry_gps(const CamTelemetryGps *t) {
    if (s_active) sidecar_on_gps(t);
}
void recorder_on_telemetry_obd(const CamTelemetryObd *t) {
    if (s_active) sidecar_on_obd(t);
}
void recorder_on_telemetry_analog(const CamTelemetryAnalog *t) {
    if (s_active) sidecar_on_analog(t);
}
void recorder_on_lap_marker(const CamLapMarker *m) {
    if (!s_active || !m) return;
    uint32_t frame = avi_writer_frame_count();
    ESP_LOGI(TAG, "LAP %u: %lu ms (sectors=%u, video frame %lu)",
             m->lap_no, (unsigned long)m->lap_ms, m->sectors_used,
             (unsigned long)frame);
    sidecar_on_lap(m, frame);
}

uint32_t recorder_get_session_bytes(void) { return avi_writer_file_size(); }
uint8_t  recorder_get_sd_free_pct(void)   { return sd_mgr_free_pct(); }
bool     recorder_has_sensor(void)        { return false; /* TODO: OV5647 detect */ }

/* ── Video index ─────────────────────────────────────────────────────── *
 *
 * Lazily scanned from /sdcard/sessions/ on demand. Cache invalidated by
 * recorder_stop() so a fresh listing always picks up the just-finished
 * session. The size is the on-disk video.avi size; gps_utc_ms_start +
 * duration are pulled out of the matching meta.json header. */

static CamVideoListEntry *s_idx_cache = NULL;
static uint32_t           s_idx_count = 0;
static bool               s_idx_dirty = true;

static uint64_t parse_uint64(const char *s) {
    if (!s) return 0;
    unsigned long long v = 0;
    sscanf(s, "%llu", &v);
    return (uint64_t)v;
}

static void parse_meta(const char *path, uint64_t *utc_out)
{
    *utc_out = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* tiny ad-hoc lookup — meta.json is something we generated, layout
     * is fixed (see write_meta_json above). */
    char *p = strstr(buf, "\"gps_utc_ms\"");
    if (p) {
        p = strchr(p, ':');
        if (p) *utc_out = parse_uint64(p + 1);
    }
}

static void rebuild_video_index(void)
{
    free(s_idx_cache);
    s_idx_cache = NULL;
    s_idx_count = 0;

    DIR *d = opendir("/sdcard/sessions");
    if (!d) { s_idx_dirty = false; return; }

    /* First pass: count entries that have a video.avi. */
    uint32_t cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char vp[160];
        snprintf(vp, sizeof(vp), "/sdcard/sessions/%s/video.avi", e->d_name);
        struct stat st;
        if (stat(vp, &st) == 0 && S_ISREG(st.st_mode)) cap++;
    }
    closedir(d);

    if (cap == 0) { s_idx_dirty = false; return; }

    s_idx_cache = (CamVideoListEntry *)heap_caps_malloc(
        cap * sizeof(CamVideoListEntry), MALLOC_CAP_SPIRAM);
    if (!s_idx_cache) { ESP_LOGE(TAG, "video idx alloc failed"); s_idx_dirty = false; return; }

    /* Second pass: populate. */
    d = opendir("/sdcard/sessions");
    if (!d) { s_idx_dirty = false; return; }
    while ((e = readdir(d)) != NULL && s_idx_count < cap) {
        if (e->d_name[0] == '.') continue;
        char vp[160], mp[160];
        snprintf(vp, sizeof(vp), "/sdcard/sessions/%s/video.avi", e->d_name);
        snprintf(mp, sizeof(mp), "/sdcard/sessions/%s/meta.json",  e->d_name);
        struct stat st;
        if (stat(vp, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        CamVideoListEntry *out = &s_idx_cache[s_idx_count++];
        memset(out, 0, sizeof(*out));
        strncpy(out->session_id, e->d_name, sizeof(out->session_id) - 1);
        out->size_bytes = (uint64_t)st.st_size;
        parse_meta(mp, &out->gps_utc_ms_start);
        /* duration_ms is unknown without parsing the AVI header; leave
         * 0 for now — Studio can derive it from video.avi metadata. */
    }
    closedir(d);
    s_idx_dirty = false;
    ESP_LOGI(TAG, "video index rebuilt: %lu entries", (unsigned long)s_idx_count);
}

const CamVideoListEntry *recorder_get_video_index(uint32_t *out_count)
{
    if (s_idx_dirty) rebuild_video_index();
    if (out_count) *out_count = s_idx_count;
    return s_idx_cache;
}
