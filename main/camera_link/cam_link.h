#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cam_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cam_link — Main-side driver for the BRL camera daughter-board (DFR1172).
 *
 * Hardware:
 *   UART2 on free header pins of the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.
 *     TX  = GPIO 30  (Header IO30)  — main TX → cam RX
 *     RX  = GPIO 31  (Header IO31)  — main RX ← cam TX
 *   115200 8N1, no flow control.  Cable also carries 5 V + GND on a 4-pin
 *   M8 connector to the camera enclosure (see README hardware table).
 *
 * Lifecycle:
 *   cam_link_init()               — call once from app_main, after NVS
 *   cam_link_poll()               — call from logic_task each iteration
 *
 * The link is purely best-effort: send-side never blocks (drops frame on
 * full TX queue), receive-side resyncs by SOF byte.  No retransmits, no
 * windowing — telemetry is high-rate and the next sample arrives anyway.
 *
 * REC START / REC STOP get an ACK back from the camera (via STATUS frame
 * with rec_active flipping), but the API doesn't block on that — UI polls
 * cam_link_get_status() to render the REC indicator.
 */

/* ── Pin / UART configuration ─────────────────────────────────────── */
#define CAM_LINK_UART_PORT  ((uart_port_t)UART_NUM_2)
#define CAM_LINK_TX_PIN     30      /* Header IO30 — main TX → cam RX */
#define CAM_LINK_RX_PIN     31      /* Header IO31 — main RX ← cam TX */
#define CAM_LINK_BAUD       115200

/* ── Lifecycle ────────────────────────────────────────────────────── */
void cam_link_init(void);
void cam_link_poll(void);

/* ── Control ──────────────────────────────────────────────────────── */
bool cam_link_rec_start(const char *session_id,
                        uint64_t   gps_utc_ms,
                        int32_t    track_idx,
                        const char *track_name,
                        const char *car_name);
bool cam_link_rec_stop(void);
bool cam_link_ping(void);

/* ── Telemetry stream (call from logic task at native sample rates) ── */
bool cam_link_send_gps(const CamTelemetryGps    *t);
bool cam_link_send_obd(const CamTelemetryObd    *t);
bool cam_link_send_lap_marker(const CamLapMarker *m);

/* ── Telemetry pump (defined in cam_link_pump.cpp) ────────────────── *
 * Reads g_state and forwards GPS / OBD samples at throttled rates while
 * a recording is active. Call once per logic_task iteration. */
void cam_link_pump_telemetry(void);

/* Build + send a CAM_FRAME_LAP_MARKER from session.laps[lap_idx]. Call
 * from lap_timer immediately after session_store_save_lap(). */
void cam_link_send_lap_marker_for(uint8_t lap_idx);

/* ── Status (kept in sync from CAM_FRAME_STATUS replies) ──────────── */
typedef struct {
    bool     link_up;          /* true while STATUS frames arrive < 2 s old */
    uint64_t last_status_ms;   /* esp_timer_get_time()/1000 of last frame   */
    CamStatusPayload status;   /* most recent STATUS payload                */
} CamLinkInfo;

CamLinkInfo cam_link_get_info(void);

/* Convenience: true iff link_up && status.rec_active. */
bool cam_link_is_recording(void);

/* ── Video index (refreshed lazily; data_server queries this) ─────── */
typedef struct {
    CamVideoListEntry *entries;   /* heap-allocated, owned by cam_link */
    uint32_t           count;
    uint64_t           refreshed_ms;
} CamVideoIndex;

/* Returns a snapshot of the current cached video list.  Caller must NOT
 * free the entries pointer.  Triggers a background refresh request to the
 * cam if the cache is older than 5 s. */
CamVideoIndex cam_link_get_video_index(void);

#ifdef __cplusplus
}
#endif
