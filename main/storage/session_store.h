#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../data/lap_data.h"
#include "../data/track_db.h"

/**
 * session_store — serialize/deserialize sessions to/from SD as JSON
 */

#ifdef __cplusplus
extern "C" {
#endif

void session_store_make_default_name(char *buf, size_t len);
void session_store_begin(const char *track_name, const char *session_name);
/// Save lap into session JSON.
void session_store_save_lap(uint8_t lap_idx);
int  session_store_list(char ids[][20], int max_count);

typedef struct {
    char     id[20];
    char     name[64];
    char     track[48];
    uint8_t  lap_count;
    uint32_t best_ms;
} SessionSummary;

int  session_store_list_summaries(SessionSummary *out, int max_count);
bool session_store_delete_session(const char *session_id);

// ---------------------------------------------------------------------------
// Saved-session lap inspection (used by "pick reference from history" UI)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  lap_num;        // 1-based, as stored in JSON
    uint32_t total_ms;
    uint32_t sector_ms[MAX_SECTORS];
    uint8_t  sectors_used;
    uint16_t point_count;    // number of track_points stored in the lap
} SessionLapInfo;

/// List laps of a saved session (summary only, no GPS points).
/// Returns number of laps filled into out[], or 0 on failure.
int  session_store_list_laps(const char *session_id,
                             SessionLapInfo *out, int max_count);

/// Load one lap of a saved session into a RecordedLap, with its track_points
/// written into the caller-supplied PSRAM buffer. points_cap is the buffer
/// capacity; loaded count is clipped to it. Returns false on any failure
/// (file missing, parse error, lap index out of range, no track_points in
/// that lap). On success out->points == points_buf and out->valid == true.
bool session_store_load_lap(const char *session_id,
                            uint8_t lap_idx_in_file,
                            RecordedLap *out_lap,
                            TrackPoint *points_buf,
                            uint16_t points_cap);

/// Scan all saved sessions for the fastest lap on the given track. Writes the
/// session id and 0-based lap index to the out parameters. Returns false when
/// no saved lap exists for that track. Only considers laps with track_points
/// (so the result is usable as a live-delta reference).
bool session_store_find_track_best(const char *track_name,
                                   char *out_session_id, size_t id_size,
                                   uint8_t *out_lap_idx,
                                   uint32_t *out_total_ms);
void session_store_load_user_tracks(void);
bool session_store_save_user_track(const TrackDef *td);
bool session_store_delete_user_track(int u_slot);
bool session_store_save_builtin_override(int builtin_idx, const TrackDef *td);
void session_store_load_builtin_overrides(void);

#ifdef __cplusplus
}
#endif
