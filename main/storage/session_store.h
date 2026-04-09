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
void session_store_load_user_tracks(void);
bool session_store_save_user_track(const TrackDef *td);
bool session_store_delete_user_track(int u_slot);
bool session_store_save_builtin_override(int builtin_idx, const TrackDef *td);
void session_store_load_builtin_overrides(void);

#ifdef __cplusplus
}
#endif
