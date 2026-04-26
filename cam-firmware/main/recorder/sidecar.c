/**
 * sidecar.c — NDJSON telemetry mirror for the cam-SD recordings.
 *
 * Appends one line per telemetry frame. Each line is a complete JSON
 * object so a partial flush at power-loss only loses the trailing
 * incomplete line — the rest of the file is still parseable.
 */

#include "sidecar.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "sidecar";

static FILE    *s_f = NULL;
static uint64_t s_anchor_ms = 0;       /* esp_timer ms at sidecar_open */
static char     s_path[200] = {};

static inline uint32_t rec_rel_ms(void)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    return (uint32_t)(now - s_anchor_ms);
}

/* fputs+fflush wrapper. NDJSON contract: callers MUST emit a complete
 * line ending in '\n', no embedded newlines, no partial writes. */
static void emit(const char *line)
{
    if (!s_f) return;
    fputs(line, s_f);
    fputc('\n', s_f);
    /* Avoid fsync per line — kills throughput. f_sync at lap markers
     * + close gives us "lose at most one lap of telemetry" durability. */
}

bool sidecar_open(const char *session_dir,
                  const char *session_id,
                  uint64_t    gps_utc_ms_anchor,
                  int32_t     track_idx,
                  const char *track_name,
                  const char *car_name)
{
    if (s_f) sidecar_close(0);

    snprintf(s_path, sizeof(s_path), "%s/telemetry.ndjson", session_dir);
    s_f = fopen(s_path, "w");
    if (!s_f) {
        ESP_LOGE(TAG, "fopen(%s) failed", s_path);
        return false;
    }
    s_anchor_ms = (uint64_t)(esp_timer_get_time() / 1000);

    char line[320];
    snprintf(line, sizeof(line),
        "{\"t\":\"session_start\",\"ms\":0,\"utc\":%" PRIu64
        ",\"id\":\"%s\",\"track_idx\":%" PRId32
        ",\"track\":\"%s\",\"car\":\"%s\"}",
        gps_utc_ms_anchor, session_id, track_idx,
        track_name ? track_name : "",
        car_name   ? car_name   : "");
    emit(line);
    fflush(s_f);

    ESP_LOGI(TAG, "open %s anchor_utc=%" PRIu64, s_path, gps_utc_ms_anchor);
    return true;
}

bool sidecar_close(uint32_t final_video_frame_count)
{
    if (!s_f) return false;

    char line[160];
    snprintf(line, sizeof(line),
        "{\"t\":\"session_end\",\"ms\":%" PRIu32 ",\"frames\":%" PRIu32 "}",
        rec_rel_ms(), final_video_frame_count);
    emit(line);
    fflush(s_f);
    fclose(s_f);
    s_f = NULL;
    ESP_LOGI(TAG, "close %s", s_path);
    return true;
}

bool sidecar_is_open(void) { return s_f != NULL; }

void sidecar_on_gps(const CamTelemetryGps *t)
{
    if (!s_f || !t) return;
    char line[256];
    snprintf(line, sizeof(line),
        "{\"t\":\"gps\",\"ms\":%" PRIu32 ",\"utc\":%" PRIu64
        ",\"lat\":%.7f,\"lon\":%.7f,\"spd\":%.2f,\"hdg\":%.1f,"
        "\"alt\":%.1f,\"hdop\":%.2f,\"sats\":%u,\"v\":%u}",
        rec_rel_ms(), t->gps_utc_ms,
        t->lat, t->lon,
        (double)t->speed_kmh, (double)t->heading_deg,
        (double)t->altitude_m, (double)t->hdop,
        (unsigned)t->satellites, (unsigned)t->valid);
    emit(line);
}

void sidecar_on_obd(const CamTelemetryObd *t)
{
    if (!s_f || !t) return;
    char line[256];
    snprintf(line, sizeof(line),
        "{\"t\":\"obd\",\"ms\":%" PRIu32 ",\"utc\":%" PRIu64
        ",\"rpm\":%.0f,\"tps\":%.1f,\"map\":%.1f,\"lam\":%.3f,"
        "\"brk\":%.1f,\"str\":%.1f,\"clt\":%.1f,\"iat\":%.1f,\"c\":%u}",
        rec_rel_ms(), t->gps_utc_ms,
        (double)t->rpm, (double)t->throttle_pct, (double)t->boost_kpa,
        (double)t->lambda, (double)t->brake_pct, (double)t->steering_angle,
        (double)t->coolant_temp_c, (double)t->intake_temp_c,
        (unsigned)t->connected);
    emit(line);
}

void sidecar_on_analog(const CamTelemetryAnalog *t)
{
    if (!s_f || !t) return;
    char line[256];
    snprintf(line, sizeof(line),
        "{\"t\":\"ana\",\"ms\":%" PRIu32 ",\"utc\":%" PRIu64
        ",\"mv\":[%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 "],"
        "\"v\":[%.4f,%.4f,%.4f,%.4f],\"mask\":%u}",
        rec_rel_ms(), t->gps_utc_ms,
        t->raw_mv[0], t->raw_mv[1], t->raw_mv[2], t->raw_mv[3],
        (double)t->value[0], (double)t->value[1],
        (double)t->value[2], (double)t->value[3],
        (unsigned)t->valid_mask);
    emit(line);
}

void sidecar_on_lap(const CamLapMarker *m, uint32_t video_frame_no)
{
    if (!s_f || !m) return;
    char line[320];
    int n = snprintf(line, sizeof(line),
        "{\"t\":\"lap\",\"ms\":%" PRIu32 ",\"utc\":%" PRIu64
        ",\"no\":%u,\"total_ms\":%" PRIu32 ",\"best\":%u,\"frame\":%" PRIu32
        ",\"sectors\":[",
        rec_rel_ms(), m->gps_utc_ms,
        (unsigned)m->lap_no, m->lap_ms, (unsigned)m->is_best_lap,
        video_frame_no);
    for (uint8_t i = 0; i < m->sectors_used && i < 8 && n < (int)sizeof(line) - 16; i++) {
        n += snprintf(line + n, sizeof(line) - n, "%s%" PRIu32,
                      (i == 0) ? "" : ",", m->sector_ms[i]);
    }
    if (n < (int)sizeof(line) - 2) {
        line[n++] = ']';
        line[n++] = '}';
        line[n] = '\0';
    }
    emit(line);
    /* Lap boundary is a natural durability checkpoint — flush so a
     * crash mid-session keeps every completed lap on disk. */
    fflush(s_f);
}
