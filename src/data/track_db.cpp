#include "track_db.h"

TrackDef g_user_tracks[MAX_USER_TRACKS] = {};
int      g_user_track_count = 0;

TrackDef g_builtin_overrides[MAX_BUILTIN_TRACKS] = {};
bool     g_builtin_override_set[MAX_BUILTIN_TRACKS] = {};
