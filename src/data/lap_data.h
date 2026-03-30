#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// GPS
// ---------------------------------------------------------------------------
typedef struct {
    double   lat;
    double   lon;
    float    speed_kmh;
    float    heading_deg;
    float    altitude_m;
    float    hdop;          // horizontal dilution of precision
    uint8_t  satellites;
    bool     valid;
} GpsData;

// ---------------------------------------------------------------------------
// OBD
// ---------------------------------------------------------------------------
typedef struct {
    float    rpm;
    float    throttle_pct;
    float    coolant_temp_c;
    float    oil_temp_c;
    int16_t  gear;          // -1 = reverse, 0 = neutral, 1-8 = gear
    bool     connected;
} ObdData;

// ---------------------------------------------------------------------------
// Lap / sector timing
// ---------------------------------------------------------------------------
#define MAX_SECTORS  8

typedef struct {
    uint32_t total_ms;
    uint32_t sector_ms[MAX_SECTORS];
    uint8_t  sectors_used;
    bool     valid;
} LapTime;

// Best lap is the fastest recorded LapTime
typedef struct {
    LapTime  best;
    LapTime  last;
    uint32_t current_lap_start_ms;   // millis() when current lap began
    uint32_t sector_start_ms;        // millis() when current sector began
    uint8_t  current_sector;
    uint16_t lap_number;
    bool     timing_active;
} LapSession;

// ---------------------------------------------------------------------------
// Application state (single global instance)
// ---------------------------------------------------------------------------
typedef struct {
    GpsData    gps;
    ObdData    obd;
    LapSession session;
    bool       wifi_connected;
    char       wifi_ssid[32];
    bool       obd_connected;
    uint8_t    language;    // 0 = DE, 1 = EN
    uint8_t    units;       // 0 = metric, 1 = imperial
    int        active_track_idx;  // -1 = none selected
} AppState;

extern AppState g_state;

// Utility: format milliseconds as "M:SS.mmm"
static inline void fmt_laptime(char *buf, size_t len, uint32_t ms) {
    uint32_t minutes = ms / 60000;
    uint32_t seconds = (ms % 60000) / 1000;
    uint32_t millis  = ms % 1000;
    snprintf(buf, len, "%lu:%02lu.%03lu", minutes, seconds, millis);
}

// Utility: format milliseconds as "+S.mmm" or "-S.mmm" (delta time)
static inline void fmt_delta(char *buf, size_t len, int32_t delta_ms) {
    const char *sign = (delta_ms >= 0) ? "+" : "-";
    uint32_t abs_ms  = (delta_ms >= 0) ? (uint32_t)delta_ms : (uint32_t)(-delta_ms);
    snprintf(buf, len, "%s%lu.%03lu", sign, abs_ms / 1000, abs_ms % 1000);
}
