#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "lap_data.h"   // MAX_SECTORS

// ---------------------------------------------------------------------------
// Sector point
// ---------------------------------------------------------------------------
#define SECTOR_NAME_LEN  16

typedef struct {
    double lat;
    double lon;
    char   name[SECTOR_NAME_LEN];  // e.g. "S1", "Eau Rouge", ""
} SectorPoint;

// ---------------------------------------------------------------------------
// Track definition
//
// S/F line: defined by two GPS points (perp. vector = crossing direction)
// Sectors:  up to MAX_SECTORS intermediate crossing lines, each also 2-point
//
// Circuit (is_circuit=true):  one S/F line, laps are full loops
// A-B stage (is_circuit=false): sf_lat1/lon1 = START, sf_lat2/lon2 = FINISH
//                                sf2_lat/lon define the perpendicular for finish
// ---------------------------------------------------------------------------
typedef struct {
    char         name[48];
    char         country[32];
    float        length_km;

    // Start/finish line — two points perpendicular to the track
    double       sf_lat1, sf_lon1;
    double       sf_lat2, sf_lon2;

    // For A-B stage: separate finish line
    double       fin_lat1, fin_lon1;
    double       fin_lat2, fin_lon2;

    // Sectors (optional, sector_count = 0 if none)
    SectorPoint  sectors[MAX_SECTORS];
    uint8_t      sector_count;

    bool         is_circuit;      // true = closed loop, false = A-B stage
    bool         user_created;    // true = created by user, stored on SD
} TrackDef;

// ---------------------------------------------------------------------------
// Built-in track database
// ---------------------------------------------------------------------------
static const TrackDef TRACK_DB[] = {
    {
        "Nürburgring GP",       "Deutschland",  5.148f,
        50.3356,  6.9475,   50.3348,  6.9490,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Nürburgring Nordschleife", "Deutschland", 20.832f,
        50.3356,  6.9475,   50.3348,  6.9490,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Hockenheimring",       "Deutschland",  4.574f,
        49.3281,  8.5656,   49.3274,  8.5665,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Red Bull Ring",        "Österreich",   4.318f,
        47.2197, 14.7648,   47.2189, 14.7658,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Spa-Francorchamps",    "Belgien",      7.004f,
        50.4372,  5.9714,   50.4380,  5.9724,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Monza",                "Italien",      5.793f,
        45.6156,  9.2811,   45.6148,  9.2820,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Silverstone GP",       "Großbritannien", 5.891f,
        52.0786, -1.0169,   52.0794, -1.0158,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Circuit de Catalunya", "Spanien",      4.655f,
        41.5700,  2.2611,   41.5708,  2.2622,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Lausitzring",          "Deutschland",  4.534f,
        51.5365, 13.9362,   51.5357, 13.9373,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        "Sachsenring",          "Deutschland",  3.645f,
        50.7936, 12.6883,   50.7928, 12.6894,
        0,0,  0,0,
        {}, 0,
        true, false
    },
};

static const int TRACK_DB_BUILTIN_COUNT = (int)(sizeof(TRACK_DB) / sizeof(TRACK_DB[0]));

// ---------------------------------------------------------------------------
// User-created tracks (loaded from SD at runtime, max 20)
// ---------------------------------------------------------------------------
#define MAX_USER_TRACKS  20

extern TrackDef   g_user_tracks[MAX_USER_TRACKS];
extern int        g_user_track_count;

// Combined access: idx 0..(BUILTIN-1) = built-in, idx BUILTIN..N = user
static inline const TrackDef *track_get(int idx) {
    if (idx < 0) return nullptr;
    if (idx < TRACK_DB_BUILTIN_COUNT) return &TRACK_DB[idx];
    int u = idx - TRACK_DB_BUILTIN_COUNT;
    if (u < g_user_track_count) return &g_user_tracks[u];
    return nullptr;
}

static inline int track_total_count() {
    return TRACK_DB_BUILTIN_COUNT + g_user_track_count;
}
