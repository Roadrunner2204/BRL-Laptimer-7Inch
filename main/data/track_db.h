#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "lap_data.h"   // MAX_SECTORS

// ---------------------------------------------------------------------------
// Sector line
//
// Two modes — chosen automatically by lap_timer:
//  (A) Single-point crosspoint (lat2==0 && lon2==0):
//      lap_timer builds an X-shape gate centered on (lat,lon). Used by all
//      built-in tracks and user-created sectors from the Android app, where
//      only a single waypoint is recorded.
//  (B) 2-point straight line (lat2!=0 || lon2!=0):
//      lap_timer treats (lat,lon) → (lat2,lon2) as one segment perpendicular
//      to the racing line. Used by .tbrl tracks imported from the VBOX
//      database — higher precision, no "which crossing direction" ambiguity.
// ---------------------------------------------------------------------------
#define SECTOR_NAME_LEN  16

typedef struct {
    double lat;                     // point 1 / single-point crosspoint
    double lon;
    char   name[SECTOR_NAME_LEN];   // e.g. "S1", "Eau Rouge", ""
    double lat2;                    // point 2 (0,0 → X-shape fallback)
    double lon2;
} SectorLine;

// Back-compat alias so older code can still compile
typedef SectorLine SectorPoint;

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
    SectorLine   sectors[MAX_SECTORS];
    uint8_t      sector_count;

    bool         is_circuit;      // true = closed loop, false = A-B stage
    bool         user_created;    // true = created by user, stored on SD
} TrackDef;

// ---------------------------------------------------------------------------
// Built-in track database — DEPRECATED
//
// No tracks are shipped in firmware anymore. The user catalog is fed
// exclusively by:
//   1. User-created tracks from the Android app → /sdcard/tracks/*.json
//   2. Encrypted bundle imported from .tbrl → loaded into PSRAM at boot
//      (see main/storage/tbrl_loader.*)
//
// The empty TRACK_DB[] + TRACK_DB_BUILTIN_COUNT=0 keep the rest of the
// codebase (track_get, builtin override storage) compiling without
// structural changes — every `idx < TRACK_DB_BUILTIN_COUNT` check just
// evaluates false and control falls through to user/bundle tiers.
// ---------------------------------------------------------------------------
static const TrackDef TRACK_DB[1] = { {} };
static const int TRACK_DB_BUILTIN_COUNT = 0;

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

// ---------------------------------------------------------------------------
// Bundle tracks (decrypted .tbrl loaded from SD at boot, PSRAM-allocated).
// Third tier after built-in + user. Pointer is nullptr until the loader
// finishes; count stays 0 until then.
// ---------------------------------------------------------------------------
extern TrackDef  *g_bundle_tracks;       // malloc'd in PSRAM, nullptr if empty
extern int        g_bundle_track_count;  // 0 if no Tracks.tbrl on SD

// Combined access order:
//   idx 0..BUILTIN-1                         = built-in (with overrides)
//   idx BUILTIN..BUILTIN+USER-1              = user-created
//   idx BUILTIN+USER..BUILTIN+USER+BUNDLE-1  = .tbrl bundle
//
// When the requested bundle track is "shadowed" by a user-created edit
// with the same name, we transparently return the user track instead —
// every path in the firmware (timing screen, lap_timer, app API, active
// track) then sees the edit, not the pristine catalog entry.
static inline const TrackDef *track_get(int idx) {
    if (idx < 0) return NULL;
    if (idx < TRACK_DB_BUILTIN_COUNT) {
        if (idx < MAX_BUILTIN_TRACKS && g_builtin_override_set[idx])
            return &g_builtin_overrides[idx];
        return &TRACK_DB[idx];
    }
    int u = idx - TRACK_DB_BUILTIN_COUNT;
    if (u < g_user_track_count) return &g_user_tracks[u];
    int b = u - g_user_track_count;
    if (g_bundle_tracks && b < g_bundle_track_count) {
        const TrackDef *bt = &g_bundle_tracks[b];
        // Transparent shadow: prefer a same-named user track if present
        for (int uu = 0; uu < g_user_track_count; uu++) {
            if (strcmp(g_user_tracks[uu].name, bt->name) == 0)
                return &g_user_tracks[uu];
        }
        return bt;
    }
    return NULL;
}

static inline int track_total_count() {
    return TRACK_DB_BUILTIN_COUNT + g_user_track_count + g_bundle_track_count;
}

// A bundle track is "shadowed" when a user-created track with the same
// name exists — the user version is the intended edit, the bundle entry
// is the pristine catalog original. Renderers (display + app) should skip
// shadowed bundle entries so the user doesn't see duplicates.
static inline bool track_is_shadowed(int idx) {
    int u_base = TRACK_DB_BUILTIN_COUNT + g_user_track_count;
    if (idx < u_base) return false;                 // not a bundle idx
    int b = idx - u_base;
    if (!g_bundle_tracks || b < 0 || b >= g_bundle_track_count) return false;
    const char *bn = g_bundle_tracks[b].name;
    if (!bn || !bn[0]) return false;
    for (int u = 0; u < g_user_track_count; u++) {
        if (strcmp(g_user_tracks[u].name, bn) == 0) return true;
    }
    return false;
}
