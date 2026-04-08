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

    // Start/finish line -- two points perpendicular to the track
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
    // -- S/F-Linie: 2 Punkte SENKRECHT zur Fahrlinie, ~10 m Breite --------
    // Format: sf_lat1/lon1 = linke Seite, sf_lat2/lon2 = rechte Seite
    // Koordinaten aus oeffentlichen GPS-Daten & OpenStreetMap (WGS84)
    {
        // Hauptgerade, Hoehe Zieldurchfahrt / pit-lane exit
        "Nürburgring GP",        "Deutschland",  5.148f,
        50.33558,  6.94930,   50.33510,  6.94975,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Nordschleife-Zeitmesslinie (Touristenfahrten / NLS) -- Doettinger Hoehe
        // Einfahrt Nordschleife, ~100 m vor der Karusell-Seite
        "Nürburgring Nordschleife", "Deutschland", 20.832f,
        50.33591,  6.94714,   50.33547,  6.94758,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Hauptgerade Mercedes-Arena, Hoehe Zielstrahl
        "Hockenheimring",        "Deutschland",  4.574f,
        49.32825,  8.56556,   49.32782,  8.56594,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Zielgerade vor dem Start/Ziel
        "Red Bull Ring",         "Österreich",   4.318f,
        47.21990, 14.76485,   47.21950, 14.76520,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Hauptgerade, Hoehe Start/Ziel-Tribuene -- S/F-Linie senkrecht zur Fahrlinie
        // S1: nach erstem Komplex (Eingang Bergkurve)
        // S2: Ende Rueckegrade vor letztem Schikanen-Komplex
        // S3: Eingang letztes Kurvenpaket vor Zielgerade
        "Salzburgring",          "Österreich",   4.241f,
        47.78340, 13.18460,   47.78320, 13.18460,
        0,0,  0,0,
        {
            { 47.78415, 13.18400, "S1" },
            { 47.78270, 13.18510, "S2" },
            { 47.78290, 13.18325, "S3" },
        }, 3,
        true, false
    },
    {
        // La Source-Seite der Hauptgeraden, Hoehe Zeitmessung
        "Spa-Francorchamps",     "Belgien",      7.004f,
        50.43718,  5.97126,   50.43677,  5.97162,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Hauptgerade vor der Variante del Rettifilo
        "Monza",                 "Italien",      5.793f,
        45.61618,  9.28118,   45.61569,  9.28155,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Wellington Straight, Zielstrahl vor dem Wing-Gebaeude
        "Silverstone GP",        "Großbritannien", 5.891f,
        52.07882, -1.01699,   52.07841, -1.01658,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Hauptgerade vor dem Zielbereich
        "Circuit de Catalunya",  "Spanien",      4.655f,
        41.57001,  2.26095,   41.56959,  2.26138,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Lausitz Eurospeedway Infield, Zielgerade
        "Lausitzring",           "Deutschland",  4.534f,
        51.53643, 13.93625,   51.53600, 13.93662,
        0,0,  0,0,
        {}, 0,
        true, false
    },
    {
        // Hauptgerade vor dem Start/Ziel
        "Sachsenring",           "Deutschland",  3.645f,
        50.79389, 12.68842,   50.79348, 12.68879,
        0,0,  0,0,
        {}, 0,
        true, false
    },
};

static const int TRACK_DB_BUILTIN_COUNT = (int)(sizeof(TRACK_DB) / sizeof(TRACK_DB[0]));

// ---------------------------------------------------------------------------
// User-created tracks (loaded from SD at runtime, max 20)
// ---------------------------------------------------------------------------
#define MAX_USER_TRACKS    20
#define MAX_BUILTIN_TRACKS 32   // upper bound for override arrays

extern TrackDef   g_user_tracks[MAX_USER_TRACKS];
extern int        g_user_track_count;

// Override storage for built-in tracks (GPS coord edits)
// Loaded from SD on boot; track_get() returns override if set.
extern TrackDef   g_builtin_overrides[MAX_BUILTIN_TRACKS];
extern bool       g_builtin_override_set[MAX_BUILTIN_TRACKS];

// Combined access: idx 0..(BUILTIN-1) = built-in, idx BUILTIN..N = user
static inline const TrackDef *track_get(int idx) {
    if (idx < 0) return nullptr;
    if (idx < TRACK_DB_BUILTIN_COUNT) {
        if (idx < MAX_BUILTIN_TRACKS && g_builtin_override_set[idx])
            return &g_builtin_overrides[idx];
        return &TRACK_DB[idx];
    }
    int u = idx - TRACK_DB_BUILTIN_COUNT;
    if (u < g_user_track_count) return &g_user_tracks[u];
    return nullptr;
}

static inline int track_total_count() {
    return TRACK_DB_BUILTIN_COUNT + g_user_track_count;
}
