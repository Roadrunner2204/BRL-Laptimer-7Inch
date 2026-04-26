#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cam_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sidecar — NDJSON telemetry mirror written next to video.avi.
 *
 * The laptimer's own SD only persists per-lap summaries (lap time,
 * sectors, downsampled GPS track-points) in /sessions/<id>.json. The
 * cam sees the live UART stream at full rate — GPS @ 10 Hz, OBD/CAN @
 * 20 Hz, analog @ 10 Hz, plus lap markers — so its SD ends up the
 * most complete record.
 *
 * Format: one JSON object per line (NDJSON / JSONL) for append-only
 * robustness. Studio reads the file as a stream; Python: `pd.read_json
 * (path, lines=True)`. Each entry has at least:
 *   {"t":"<kind>","ms":<recorder-relative ms>,"utc":<gps_utc_ms>,…}
 *
 * Kinds: "gps", "obd", "ana", "lap", "session_start", "session_end".
 */

bool sidecar_open(const char           *session_dir,
                  const char           *session_id,
                  uint64_t              gps_utc_ms_anchor,
                  int32_t               track_idx,
                  const char           *track_name,
                  const char           *car_name);
bool sidecar_close(uint32_t final_video_frame_count);
bool sidecar_is_open(void);

void sidecar_on_gps   (const CamTelemetryGps    *t);
void sidecar_on_obd   (const CamTelemetryObd    *t);
void sidecar_on_analog(const CamTelemetryAnalog *t);
void sidecar_on_lap   (const CamLapMarker       *m, uint32_t video_frame_no);

#ifdef __cplusplus
}
#endif
