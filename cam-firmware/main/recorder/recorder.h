#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cam_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * recorder — orchestrates one recording session on the cam module.
 *
 * Per session, on the local microSD:
 *   /sessions/<session_id>/video.avi      MJPEG-AVI from MIPI-CSI capture
 *   /sessions/<session_id>/telemetry.brl  Mirror of laptimer telemetry
 *                                          (same .brl format the laptimer
 *                                          writes to its own SD)
 *   /sessions/<session_id>/sync.json      session_id, GPS-UTC anchor,
 *                                          per-lap {video_pts ↔ gps_utc}
 *   /sessions/<session_id>/meta.json      track, car, cam settings
 *
 * Telemetry mirroring is what makes the offline workflow possible — if
 * the user never connects the laptimer to a phone or laptop, they can
 * still pull the cam-SD and Studio reads all the data from this folder.
 */

typedef struct {
    char     session_id[20];
    uint64_t gps_utc_ms;
    int32_t  track_idx;
    char     track_name[32];
    char     car_name[32];
} RecorderSessionInfo;

void recorder_init(void);
void recorder_poll(void);

bool recorder_start(const RecorderSessionInfo *info);
bool recorder_stop(void);
bool recorder_is_active(void);

/* Telemetry mirror — call from the cam_link frame dispatcher. */
void recorder_on_telemetry_gps(const CamTelemetryGps    *t);
void recorder_on_telemetry_obd(const CamTelemetryObd    *t);
void recorder_on_telemetry_analog(const CamTelemetryAnalog *t);
void recorder_on_lap_marker(const CamLapMarker        *m);

/* Status helpers (used by the periodic STATUS reply). */
uint32_t recorder_get_session_bytes(void);
uint8_t  recorder_get_sd_free_pct(void);
bool     recorder_has_sensor(void);

/* Video index for CAM_FRAME_VIDEO_LIST replies. Caller does NOT free. */
const CamVideoListEntry *recorder_get_video_index(uint32_t *out_count);

#ifdef __cplusplus
}
#endif
