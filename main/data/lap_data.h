#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Mutex protecting g_state across Core 0 (logic) and Core 1 (LVGL).
// Defined in main.cpp; include this header to access it.
extern SemaphoreHandle_t g_state_mutex;

// ---------------------------------------------------------------------------
// GPS
// ---------------------------------------------------------------------------
typedef struct {
    double   lat;
    double   lon;
    float    speed_kmh;
    float    heading_deg;
    float    altitude_m;
    float    hdop;
    uint8_t  satellites;
    bool     valid;
} GpsData;

// ---------------------------------------------------------------------------
// OBD
// ---------------------------------------------------------------------------
typedef struct {
    float    rpm;
    float    throttle_pct;    // 0-100 %
    float    boost_kpa;       // MAP absolute; subtract ~101 kPa for boost
    float    lambda;          // 1.0 = stoich
    float    brake_pct;       // 0-100 % (custom BRL PID 0xA0)
    float    steering_angle;  // degrees, +/-540 (custom BRL PID 0xA1)
    float    coolant_temp_c;
    float    intake_temp_c;
    bool     connected;
} ObdData;

// ---------------------------------------------------------------------------
// GPS track recording
// ---------------------------------------------------------------------------
#define MAX_TRACK_POINTS  8000   // ~8 min at 10 Hz, stored in PSRAM

typedef struct {
    double   lat;
    double   lon;
    uint32_t lap_ms;   // elapsed ms from lap start at this point
} TrackPoint;

// ---------------------------------------------------------------------------
// Recorded lap (PSRAM-backed)
// ---------------------------------------------------------------------------
#define MAX_SECTORS  8

typedef struct {
    uint32_t    total_ms;
    uint32_t    sector_ms[MAX_SECTORS];
    uint8_t     sectors_used;
    TrackPoint *points;        // pointer into PSRAM array, nullptr if invalid
    uint16_t    point_count;
    bool        valid;
} RecordedLap;

// ---------------------------------------------------------------------------
// Session: one session on one track (multiple laps)
// ---------------------------------------------------------------------------
#define MAX_LAPS_PER_SESSION  50

typedef struct {
    RecordedLap laps[MAX_LAPS_PER_SESSION];
    uint8_t     lap_count;
    uint8_t     best_lap_idx;      // index of fastest valid lap
    uint8_t     ref_lap_idx;       // user-chosen reference (default = best)
    int         track_idx;         // index into track database
    uint32_t    session_start_epoch; // Unix time if available
    char        session_id[20];    // "YYYYMMDD_HHMMSS"
} LapSession;

// ---------------------------------------------------------------------------
// Live timing state (per-lap, reset each crossing)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t lap_start_ms;       // millis() at lap start
    uint32_t sector_start_ms;    // millis() at sector start
    uint8_t  current_sector;
    uint16_t lap_number;         // total laps this session (0 = not yet started)
    bool     timing_active;      // true after first S/F crossing
    bool     in_lap;             // currently recording a lap
    int32_t  live_delta_ms;      // vs reference lap at current position
    uint16_t ref_point_idx;      // search cursor in reference lap
} LiveTiming;

// ---------------------------------------------------------------------------
// WiFi mode  (prefixed BRL_ to avoid collision with Arduino WiFi.h macros)
// ---------------------------------------------------------------------------
typedef enum {
    BRL_WIFI_OFF = 0,
    BRL_WIFI_AP,       // host AP -- Android app connects
    BRL_WIFI_STA,      // connect to external network
    BRL_WIFI_OTA       // OTA firmware update in progress
} WifiMode;

// ---------------------------------------------------------------------------
// Vehicle data connection mode
// ---------------------------------------------------------------------------
typedef enum {
    VEH_CONN_OBD_BLE = 0,   // OBD-II via BRL BLE Adapter (default)
    VEH_CONN_CAN_DIRECT,    // Direct CAN bus via SN65HVD230 transceiver
} VehicleConnMode;

// ---------------------------------------------------------------------------
// Application state (single global instance)
// ---------------------------------------------------------------------------
typedef struct {
    GpsData    gps;
    ObdData    obd;
    LapSession session;
    LiveTiming timing;

    WifiMode       wifi_mode;
    char           wifi_ssid[32];
    bool           sd_available;

    VehicleConnMode veh_conn_mode;  // OBD BLE or direct CAN
    bool           obd_connected;
    bool           camera_connected;
    bool           video_recording;
    uint8_t        language;   // 0 = DE, 1 = EN
    uint8_t        units;      // 0 = metric, 1 = imperial
    int            active_track_idx;  // -1 = none selected
} AppState;

extern AppState g_state;

// ---------------------------------------------------------------------------
// Utility: format milliseconds as "M:SS.mmm"
// ---------------------------------------------------------------------------
static inline void fmt_laptime(char *buf, size_t len, uint32_t ms) {
    uint32_t minutes = ms / 60000;
    uint32_t seconds = (ms % 60000) / 1000;
    uint32_t millis_ = ms % 1000;
    snprintf(buf, len, "%lu:%02lu.%03lu",
             (unsigned long)minutes,
             (unsigned long)seconds,
             (unsigned long)millis_);
}

// ---------------------------------------------------------------------------
// Utility: format delta as "+S.mmm" or "-S.mmm"
// ---------------------------------------------------------------------------
static inline void fmt_delta(char *buf, size_t len, int32_t delta_ms) {
    const char *sign = (delta_ms >= 0) ? "+" : "-";
    uint32_t abs_ms  = (delta_ms >= 0) ? (uint32_t)delta_ms : (uint32_t)(-delta_ms);
    snprintf(buf, len, "%s%lu.%03lu",
             sign,
             (unsigned long)(abs_ms / 1000),
             (unsigned long)(abs_ms % 1000));
}
