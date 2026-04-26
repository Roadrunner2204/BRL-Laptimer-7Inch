#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BRL Laptimer ↔ Camera-Module wire protocol.
 *
 * This header is the single source of truth for the binary framing used on
 * the UART link between the main laptimer (ESP32-P4 Waveshare 7B) and the
 * external camera module (DFR1172 + OV5647). It is intentionally
 * dependency-free so the cam-firmware repo can include this exact file
 * verbatim — keep it ANSI-C, no FreeRTOS / no LVGL / no g_state.
 *
 *   Frame layout
 *   ────────────
 *     [SOF=0xA5] [TYPE u8] [LEN u16-LE] [PAYLOAD ... LEN bytes] [CRC8]
 *
 *     SOF   constant 0xA5 — receiver hunts for this byte to resync
 *     TYPE  one of CAM_FRAME_* enum values below
 *     LEN   payload length in bytes (excludes header + CRC), little-endian
 *     CRC   CRC-8/MAXIM (poly 0x31, init 0x00) over TYPE | LEN-LE | PAYLOAD
 *
 *   Direction
 *   ─────────
 *     M→C   main laptimer → camera module
 *     C→M   camera module → main laptimer
 */

#define CAM_LINK_SOF          0xA5
#define CAM_LINK_PROTO_VER    1     /* bump on incompatible payload changes */

/* Maximum payload bytes per frame (sized so a frame fits in one UART chunk). */
#define CAM_LINK_MAX_PAYLOAD  256

typedef enum {
    /* ── Control: M→C ───────────────────────────────────────────── */
    CAM_FRAME_REC_START   = 0x01,   /* payload: CamRecStartPayload  */
    CAM_FRAME_REC_STOP    = 0x02,   /* payload: empty               */
    CAM_FRAME_PING        = 0x03,   /* payload: empty (link probe)  */

    /* ── Telemetry stream: M→C ──────────────────────────────────── */
    CAM_FRAME_TELE_GPS    = 0x10,   /* payload: CamTelemetryGps     */
    CAM_FRAME_TELE_OBD    = 0x11,   /* payload: CamTelemetryObd     */
    CAM_FRAME_TELE_ANALOG = 0x12,   /* payload: CamTelemetryAnalog  */
    CAM_FRAME_LAP_MARKER  = 0x20,   /* payload: CamLapMarker        */

    /* ── Replies: C→M ───────────────────────────────────────────── */
    CAM_FRAME_STATUS      = 0x80,   /* payload: CamStatusPayload    */
    CAM_FRAME_ACK         = 0x81,   /* payload: { u8 ack_for_type } */
    CAM_FRAME_NACK        = 0x82,   /* payload: { u8 nack_for_type, u8 reason } */
    CAM_FRAME_VIDEO_LIST  = 0x90,   /* payload: CamVideoListEntry[] */
} CamFrameType;

/* ────────── Payload structs (all little-endian, packed) ────────── */

#pragma pack(push, 1)

typedef struct {
    char     session_id[20];   /* "YYYYMMDD_HHMMSS\0" */
    uint64_t gps_utc_ms;       /* UTC anchor at REC start, ms since epoch */
    int32_t  track_idx;        /* index into track DB, -1 if unknown      */
    char     car_name[32];     /* active car profile name                  */
    char     track_name[32];   /* human-readable track label              */
    uint8_t  proto_ver;        /* CAM_LINK_PROTO_VER                       */
    uint8_t  reserved[7];
} CamRecStartPayload;

typedef struct {
    uint64_t gps_utc_ms;       /* sample timestamp, UTC ms since epoch     */
    double   lat;
    double   lon;
    float    speed_kmh;
    float    heading_deg;
    float    altitude_m;
    float    hdop;
    uint8_t  satellites;
    uint8_t  valid;            /* 0/1, sized as u8 to keep alignment       */
    uint8_t  reserved[6];
} CamTelemetryGps;

typedef struct {
    uint64_t gps_utc_ms;
    float    rpm;
    float    throttle_pct;
    float    boost_kpa;
    float    lambda;
    float    brake_pct;
    float    steering_angle;
    float    coolant_temp_c;
    float    intake_temp_c;
    uint8_t  connected;
    uint8_t  reserved[7];
} CamTelemetryObd;

typedef struct {
    uint64_t gps_utc_ms;
    int32_t  raw_mv[4];
    float    value[4];
    uint8_t  valid_mask;       /* bit i = channel i valid                  */
    uint8_t  reserved[7];
} CamTelemetryAnalog;

typedef struct {
    uint64_t gps_utc_ms;       /* lap-end timestamp                        */
    uint16_t lap_no;
    uint16_t reserved0;
    uint32_t lap_ms;
    uint32_t sector_ms[8];
    uint8_t  sectors_used;
    uint8_t  is_best_lap;
    uint8_t  reserved[6];
} CamLapMarker;

typedef struct {
    uint8_t  rec_active;       /* 0/1 */
    uint8_t  sd_free_pct;      /* 0..100 */
    uint8_t  cam_connected;    /* 0/1 — OV5647 detected at boot */
    uint8_t  wifi_sta_up;      /* 0/1 — joined laptimer AP */
    uint16_t err_code;         /* 0 = OK, otherwise CamErrorCode */
    uint16_t reserved0;
    uint32_t cur_session_bytes;/* bytes written for current session */
    uint64_t uptime_ms;
    char     ip_addr[16];      /* dotted IPv4, "0.0.0.0" if not connected */
    uint16_t http_port;        /* HTTP server port (typ. 80) */
    uint8_t  reserved[6];
} CamStatusPayload;

typedef struct {
    char     session_id[20];   /* matches REC_START session_id */
    uint64_t size_bytes;
    uint64_t gps_utc_ms_start;
    uint32_t duration_ms;
    uint8_t  reserved[8];
} CamVideoListEntry;

#pragma pack(pop)

typedef enum {
    CAM_ERR_OK             = 0,
    CAM_ERR_NO_SD          = 1,
    CAM_ERR_SD_FULL        = 2,
    CAM_ERR_SD_WRITE_FAIL  = 3,
    CAM_ERR_NO_SENSOR      = 4,
    CAM_ERR_PROTO_MISMATCH = 5,
} CamErrorCode;

/* ────────── CRC-8/MAXIM helper (also used by cam-firmware) ────────── */
static inline uint8_t cam_link_crc8(const uint8_t *data, uint32_t len) {
    uint8_t crc = 0x00;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

#ifdef __cplusplus
}
#endif
