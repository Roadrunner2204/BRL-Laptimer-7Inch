#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../data/lap_data.h"
#include "../data/track_db.h"

/**
 * session_store — serialize/deserialize sessions to/from SD as JSON
 *
 * File layout on SD:
 *   /sessions/<session_id>.json
 *
 * JSON format (example):
 * {
 *   "id": "20240315_143022",
 *   "track": "Nürburgring GP",
 *   "laps": [
 *     { "lap": 1, "total_ms": 125432, "sectors": [41200, 42100, 42132],
 *       "track_points": [ [lat, lon, ms], ... ] },
 *     ...
 *   ]
 * }
 *
 * Track points are only written if GPS track was recorded.
 * ArduinoJson 7 is used for serialization.
 */

// Generate a session ID from current time (uses millis if no RTC)
void session_store_begin(const char *track_name);

// Save one lap to the session JSON on SD (called after each lap finishes)
void session_store_save_lap(uint8_t lap_idx);

// Load the list of session IDs from SD into a buffer
// Returns number of entries written
int  session_store_list(char ids[][20], int max_count);

// Load user-created tracks from SD into g_user_tracks
void session_store_load_user_tracks();

// Save a user-created track to SD
bool session_store_save_user_track(const TrackDef *td);
