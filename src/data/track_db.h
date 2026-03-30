#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Track definition
// Start/finish line is defined as the midpoint between two GPS coordinates.
// A lap is counted when the GPS track crosses that line.
// ---------------------------------------------------------------------------
typedef struct {
    const char *name;
    const char *country;
    float       length_km;
    // Start/finish line — two points defining the line
    double      sf_lat1;
    double      sf_lon1;
    double      sf_lat2;
    double      sf_lon2;
} TrackInfo;

// ---------------------------------------------------------------------------
// Built-in track database
// Coordinates are approximate start/finish line positions.
// ---------------------------------------------------------------------------
static const TrackInfo TRACK_DB[] = {
    {
        "Nürburgring GP",       "Deutschland",  5.148,
        50.3356,  6.9475,   50.3348,  6.9490
    },
    {
        "Nürburgring Nordschleife", "Deutschland", 20.832,
        50.3356,  6.9475,   50.3348,  6.9490
    },
    {
        "Hockenheimring",       "Deutschland",  4.574,
        49.3281,  8.5656,   49.3274,  8.5665
    },
    {
        "Red Bull Ring",        "Österreich",   4.318,
        47.2197, 14.7648,   47.2189, 14.7658
    },
    {
        "Spa-Francorchamps",    "Belgien",      7.004,
        50.4372,  5.9714,   50.4380,  5.9724
    },
    {
        "Monza",                "Italien",      5.793,
        45.6156,  9.2811,   45.6148,  9.2820
    },
    {
        "Silverstone GP",       "Großbritannien", 5.891,
        52.0786, -1.0169,   52.0794, -1.0158
    },
    {
        "Circuit de Catalunya", "Spanien",      4.655,
        41.5700,  2.2611,   41.5708,  2.2622
    },
    {
        "Lausitzring",          "Deutschland",  4.534,
        51.5365, 13.9362,   51.5357, 13.9373
    },
    {
        "Sachsenring",          "Deutschland",  3.645,
        50.7936, 12.6883,   50.7928, 12.6894
    },
};

static const int TRACK_DB_COUNT = (int)(sizeof(TRACK_DB) / sizeof(TRACK_DB[0]));
