/**
 * session_store.cpp -- Session & track persistence for ESP-IDF / ESP32-P4
 *
 * Stores sessions as JSON files on the SD card:
 *   /sdcard/sessions/<session_id>.json
 *
 * User tracks:
 *   /sdcard/tracks/user_<N>.json       (N = 0 .. MAX_USER_TRACKS-1)
 *
 * Built-in track overrides:
 *   /sdcard/tracks/override_<N>.json   (N = builtin index)
 *
 * Uses cJSON (bundled with ESP-IDF) for serialisation and the sd_mgr
 * POSIX wrappers for all file I/O.
 */

#include "session_store.h"
#include "sd_mgr.h"
#include "compat.h"
#include "cJSON.h"

#include "../gps/gps.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"

#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "session_store";

#define SESSIONS_DIR  "/sessions"
#define TRACKS_DIR    "/tracks"
#define READ_BUF_SIZE 8192

// ---------------------------------------------------------------------------
// Helper: build a path for a session file
// ---------------------------------------------------------------------------
static void session_path(char *out, size_t len, const char *session_id)
{
    snprintf(out, len, "%s/%s.json", SESSIONS_DIR, session_id);
}

// ---------------------------------------------------------------------------
// session_store_make_default_name -- e.g. "20260408_143022" from GPS time
// ---------------------------------------------------------------------------
void session_store_make_default_name(char *buf, size_t len)
{
    GpsDateTime dt = gps_get_datetime();
    if (dt.valid) {
        snprintf(buf, len, "%04u%02u%02u_%02u%02u%02u",
                 dt.year, dt.month, dt.day,
                 dt.hour, dt.minute, dt.second);
    } else {
        snprintf(buf, len, "session_%lu", (unsigned long)millis());
    }
}

// ---------------------------------------------------------------------------
// session_store_begin -- create session JSON and write to SD
// ---------------------------------------------------------------------------
void session_store_begin(const char *track_name, const char *session_name)
{
    if (!sd_mgr_available()) {
        log_w("SD not available, cannot begin session");
        return;
    }

    LapSession &sess = g_state.session;

    // Use session_name as id (it's typically the GPS timestamp string)
    strncpy(sess.session_id, session_name, sizeof(sess.session_id) - 1);
    sess.session_id[sizeof(sess.session_id) - 1] = '\0';
    sess.lap_count     = 0;
    sess.best_lap_idx  = 0;
    sess.ref_lap_idx   = 0;

    cJSON *root = cJSON_CreateObject();
    if (!root) { log_e("cJSON alloc failed"); return; }

    cJSON_AddStringToObject(root, "id",    sess.session_id);
    cJSON_AddStringToObject(root, "name",  session_name);
    cJSON_AddStringToObject(root, "track", track_name ? track_name : "");
    cJSON_AddNumberToObject(root, "track_idx", g_state.active_track_idx);
    cJSON_AddItemToObject(root, "laps", cJSON_CreateArray());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) { log_e("cJSON_Print failed"); return; }

    char path[128];
    session_path(path, sizeof(path), sess.session_id);

    sd_write_file(path, json, strlen(json));
    free(json);

    log_i("Session started: %s (%s)", sess.session_id,
          track_name ? track_name : "no track");
}

// ---------------------------------------------------------------------------
// session_store_save_lap -- append lap N to the session JSON on SD
// ---------------------------------------------------------------------------
void session_store_save_lap(uint8_t lap_idx)
{
    if (!sd_mgr_available()) return;

    const LapSession &sess = g_state.session;
    if (lap_idx >= sess.lap_count) return;
    const RecordedLap &rl = sess.laps[lap_idx];
    if (!rl.valid) return;

    // Read existing session JSON
    char *buf = (char *)malloc(READ_BUF_SIZE);
    if (!buf) { log_e("malloc failed"); return; }

    char path[128];
    session_path(path, sizeof(path), sess.session_id);

    if (!sd_read_file(path, buf, READ_BUF_SIZE)) {
        log_e("Cannot read session file %s", path);
        free(buf);
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { log_e("JSON parse failed for %s", path); return; }

    cJSON *laps = cJSON_GetObjectItem(root, "laps");
    if (!laps) {
        laps = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "laps", laps);
    }

    // Build lap object
    cJSON *lap = cJSON_CreateObject();
    cJSON_AddNumberToObject(lap, "total_ms", rl.total_ms);
    cJSON_AddNumberToObject(lap, "sectors_used", rl.sectors_used);

    if (rl.sectors_used > 0) {
        cJSON *sec_arr = cJSON_CreateArray();
        for (int s = 0; s < rl.sectors_used && s < MAX_SECTORS; s++) {
            cJSON_AddItemToArray(sec_arr, cJSON_CreateNumber(rl.sector_ms[s]));
        }
        cJSON_AddItemToObject(lap, "sector_ms", sec_arr);
    }

    cJSON_AddItemToArray(laps, lap);

    // Update best lap info in root
    cJSON_DeleteItemFromObject(root, "best_lap_idx");
    cJSON_AddNumberToObject(root, "best_lap_idx", sess.best_lap_idx);
    cJSON_DeleteItemFromObject(root, "lap_count");
    cJSON_AddNumberToObject(root, "lap_count", sess.lap_count);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        sd_write_file(path, json, strlen(json));
        free(json);
        log_i("Saved lap %d to %s", lap_idx + 1, path);
    }
}

// ---------------------------------------------------------------------------
// session_store_list -- list session IDs from the /sessions directory
// ---------------------------------------------------------------------------
int session_store_list(char ids[][20], int max_count)
{
    if (!sd_mgr_available()) return 0;

    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/sdcard%s", SESSIONS_DIR);

    DIR *d = opendir(dir_path);
    if (!d) { log_w("Cannot open %s", dir_path); return 0; }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_count) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6) continue;  // need at least "x.json"
        // Check .json extension
        if (strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        // Strip ".json" to get session ID
        size_t id_len = nlen - 5;
        if (id_len >= 20) id_len = 19;
        memcpy(ids[count], ent->d_name, id_len);
        ids[count][id_len] = '\0';
        count++;
    }
    closedir(d);
    return count;
}

// ---------------------------------------------------------------------------
// session_store_list_summaries -- build SessionSummary array from SD
// ---------------------------------------------------------------------------
int session_store_list_summaries(SessionSummary *out, int max_count)
{
    if (!sd_mgr_available()) return 0;

    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/sdcard%s", SESSIONS_DIR);

    DIR *d = opendir(dir_path);
    if (!d) { log_w("Cannot open %s", dir_path); return 0; }

    char *buf = (char *)malloc(READ_BUF_SIZE);
    if (!buf) { closedir(d); return 0; }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_count) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0)
            continue;

        // Read and parse the session file
        char fpath[128];
        snprintf(fpath, sizeof(fpath), "%s/%s", SESSIONS_DIR, ent->d_name);

        if (!sd_read_file(fpath, buf, READ_BUF_SIZE)) continue;

        cJSON *root = cJSON_Parse(buf);
        if (!root) continue;

        SessionSummary &ss = out[count];
        memset(&ss, 0, sizeof(ss));

        // id
        cJSON *j_id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(j_id)) {
            strncpy(ss.id, j_id->valuestring, sizeof(ss.id) - 1);
        } else {
            // Derive from filename
            size_t id_len = nlen - 5;
            if (id_len >= sizeof(ss.id)) id_len = sizeof(ss.id) - 1;
            memcpy(ss.id, ent->d_name, id_len);
            ss.id[id_len] = '\0';
        }

        // name
        cJSON *j_name = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(j_name))
            strncpy(ss.name, j_name->valuestring, sizeof(ss.name) - 1);

        // track
        cJSON *j_track = cJSON_GetObjectItem(root, "track");
        if (cJSON_IsString(j_track))
            strncpy(ss.track, j_track->valuestring, sizeof(ss.track) - 1);

        // lap_count & best_ms from laps array
        cJSON *j_laps = cJSON_GetObjectItem(root, "laps");
        if (cJSON_IsArray(j_laps)) {
            ss.lap_count = (uint8_t)cJSON_GetArraySize(j_laps);
            uint32_t best = UINT32_MAX;
            cJSON *lap_item;
            cJSON_ArrayForEach(lap_item, j_laps) {
                cJSON *j_ms = cJSON_GetObjectItem(lap_item, "total_ms");
                if (cJSON_IsNumber(j_ms)) {
                    uint32_t ms = (uint32_t)j_ms->valuedouble;
                    if (ms > 0 && ms < best) best = ms;
                }
            }
            ss.best_ms = (best == UINT32_MAX) ? 0 : best;
        }

        cJSON_Delete(root);
        count++;
    }

    free(buf);
    closedir(d);
    return count;
}

// ---------------------------------------------------------------------------
// User tracks
// ---------------------------------------------------------------------------
static void user_track_path(char *out, size_t len, int slot)
{
    snprintf(out, len, "%s/user_%d.json", TRACKS_DIR, slot);
}

// Serialise a TrackDef to cJSON
static cJSON *trackdef_to_json(const TrackDef *td)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "name",    td->name);
    cJSON_AddStringToObject(obj, "country", td->country);
    cJSON_AddNumberToObject(obj, "length_km", td->length_km);

    cJSON_AddNumberToObject(obj, "sf_lat1", td->sf_lat1);
    cJSON_AddNumberToObject(obj, "sf_lon1", td->sf_lon1);
    cJSON_AddNumberToObject(obj, "sf_lat2", td->sf_lat2);
    cJSON_AddNumberToObject(obj, "sf_lon2", td->sf_lon2);

    cJSON_AddNumberToObject(obj, "fin_lat1", td->fin_lat1);
    cJSON_AddNumberToObject(obj, "fin_lon1", td->fin_lon1);
    cJSON_AddNumberToObject(obj, "fin_lat2", td->fin_lat2);
    cJSON_AddNumberToObject(obj, "fin_lon2", td->fin_lon2);

    cJSON_AddBoolToObject(obj, "is_circuit",   td->is_circuit);
    cJSON_AddBoolToObject(obj, "user_created", td->user_created);
    cJSON_AddNumberToObject(obj, "sector_count", td->sector_count);

    if (td->sector_count > 0) {
        cJSON *sec_arr = cJSON_CreateArray();
        for (int i = 0; i < td->sector_count && i < MAX_SECTORS; i++) {
            cJSON *sp = cJSON_CreateObject();
            cJSON_AddNumberToObject(sp, "lat", td->sectors[i].lat);
            cJSON_AddNumberToObject(sp, "lon", td->sectors[i].lon);
            cJSON_AddStringToObject(sp, "name", td->sectors[i].name);
            cJSON_AddItemToArray(sec_arr, sp);
        }
        cJSON_AddItemToObject(obj, "sectors", sec_arr);
    }

    return obj;
}

// Deserialise cJSON into a TrackDef
static bool json_to_trackdef(cJSON *obj, TrackDef *td)
{
    if (!obj || !td) return false;
    memset(td, 0, sizeof(*td));

    cJSON *j;

    j = cJSON_GetObjectItem(obj, "name");
    if (cJSON_IsString(j))
        strncpy(td->name, j->valuestring, sizeof(td->name) - 1);

    j = cJSON_GetObjectItem(obj, "country");
    if (cJSON_IsString(j))
        strncpy(td->country, j->valuestring, sizeof(td->country) - 1);

    j = cJSON_GetObjectItem(obj, "length_km");
    if (cJSON_IsNumber(j))
        td->length_km = (float)j->valuedouble;

    j = cJSON_GetObjectItem(obj, "sf_lat1");
    if (cJSON_IsNumber(j)) td->sf_lat1 = j->valuedouble;
    j = cJSON_GetObjectItem(obj, "sf_lon1");
    if (cJSON_IsNumber(j)) td->sf_lon1 = j->valuedouble;
    j = cJSON_GetObjectItem(obj, "sf_lat2");
    if (cJSON_IsNumber(j)) td->sf_lat2 = j->valuedouble;
    j = cJSON_GetObjectItem(obj, "sf_lon2");
    if (cJSON_IsNumber(j)) td->sf_lon2 = j->valuedouble;

    j = cJSON_GetObjectItem(obj, "fin_lat1");
    if (cJSON_IsNumber(j)) td->fin_lat1 = j->valuedouble;
    j = cJSON_GetObjectItem(obj, "fin_lon1");
    if (cJSON_IsNumber(j)) td->fin_lon1 = j->valuedouble;
    j = cJSON_GetObjectItem(obj, "fin_lat2");
    if (cJSON_IsNumber(j)) td->fin_lat2 = j->valuedouble;
    j = cJSON_GetObjectItem(obj, "fin_lon2");
    if (cJSON_IsNumber(j)) td->fin_lon2 = j->valuedouble;

    j = cJSON_GetObjectItem(obj, "is_circuit");
    td->is_circuit = cJSON_IsTrue(j);

    j = cJSON_GetObjectItem(obj, "user_created");
    td->user_created = cJSON_IsTrue(j);

    j = cJSON_GetObjectItem(obj, "sector_count");
    if (cJSON_IsNumber(j))
        td->sector_count = (uint8_t)j->valuedouble;
    if (td->sector_count > MAX_SECTORS)
        td->sector_count = MAX_SECTORS;

    cJSON *sec_arr = cJSON_GetObjectItem(obj, "sectors");
    if (cJSON_IsArray(sec_arr)) {
        int n = cJSON_GetArraySize(sec_arr);
        if (n > MAX_SECTORS) n = MAX_SECTORS;
        for (int i = 0; i < n; i++) {
            cJSON *sp = cJSON_GetArrayItem(sec_arr, i);
            if (!sp) continue;

            j = cJSON_GetObjectItem(sp, "lat");
            if (cJSON_IsNumber(j)) td->sectors[i].lat = j->valuedouble;

            j = cJSON_GetObjectItem(sp, "lon");
            if (cJSON_IsNumber(j)) td->sectors[i].lon = j->valuedouble;

            j = cJSON_GetObjectItem(sp, "name");
            if (cJSON_IsString(j))
                strncpy(td->sectors[i].name, j->valuestring,
                        SECTOR_NAME_LEN - 1);
        }
        td->sector_count = (uint8_t)n;
    }

    return true;
}

// ---------------------------------------------------------------------------
// session_store_load_user_tracks -- load all user_<N>.json into g_user_tracks
// ---------------------------------------------------------------------------
void session_store_load_user_tracks()
{
    if (!sd_mgr_available()) return;

    g_user_track_count = 0;
    char *buf = (char *)malloc(READ_BUF_SIZE);
    if (!buf) return;

    for (int i = 0; i < MAX_USER_TRACKS; i++) {
        char path[128];
        user_track_path(path, sizeof(path), i);

        if (!sd_file_exists(path)) continue;
        if (!sd_read_file(path, buf, READ_BUF_SIZE)) continue;

        cJSON *root = cJSON_Parse(buf);
        if (!root) continue;

        if (json_to_trackdef(root, &g_user_tracks[g_user_track_count])) {
            g_user_tracks[g_user_track_count].user_created = true;
            g_user_track_count++;
        }
        cJSON_Delete(root);
    }

    free(buf);
    log_i("Loaded %d user tracks from SD", g_user_track_count);
}

// ---------------------------------------------------------------------------
// session_store_save_user_track -- write a user track to SD
// ---------------------------------------------------------------------------
bool session_store_save_user_track(const TrackDef *td)
{
    if (!sd_mgr_available() || !td) return false;

    // Find the slot: match by name, or use the next free slot
    int slot = -1;
    for (int i = 0; i < g_user_track_count; i++) {
        if (strcmp(g_user_tracks[i].name, td->name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_user_track_count >= MAX_USER_TRACKS) {
            log_e("Max user tracks reached");
            return false;
        }
        slot = g_user_track_count;
    }

    cJSON *obj = trackdef_to_json(td);
    if (!obj) return false;

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return false;

    char path[128];
    user_track_path(path, sizeof(path), slot);

    bool ok = sd_write_file(path, json, strlen(json));
    free(json);

    if (ok) {
        log_i("Saved user track slot %d: %s", slot, td->name);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// session_store_delete_user_track -- remove user track file and compact array
// ---------------------------------------------------------------------------
bool session_store_delete_user_track(int u_slot)
{
    if (!sd_mgr_available()) return false;
    if (u_slot < 0 || u_slot >= g_user_track_count) return false;

    // Delete the file for this slot
    char path[128];
    user_track_path(path, sizeof(path), u_slot);
    sd_delete_file(path);

    // Compact: shift entries down
    for (int i = u_slot; i < g_user_track_count - 1; i++) {
        g_user_tracks[i] = g_user_tracks[i + 1];
    }
    g_user_track_count--;
    memset(&g_user_tracks[g_user_track_count], 0, sizeof(TrackDef));

    // Re-save all remaining tracks to keep filenames sequential
    for (int i = u_slot; i < g_user_track_count; i++) {
        cJSON *obj = trackdef_to_json(&g_user_tracks[i]);
        if (!obj) continue;
        char *json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (!json) continue;

        char p[128];
        user_track_path(p, sizeof(p), i);
        sd_write_file(p, json, strlen(json));
        free(json);
    }

    // Delete the now-stale last slot file
    char last_path[128];
    user_track_path(last_path, sizeof(last_path), g_user_track_count);
    sd_delete_file(last_path);   // may not exist, ignore error

    log_i("Deleted user track slot %d, %d remaining", u_slot, g_user_track_count);
    return true;
}

// ---------------------------------------------------------------------------
// Built-in track overrides
// ---------------------------------------------------------------------------
static void override_path(char *out, size_t len, int builtin_idx)
{
    snprintf(out, len, "%s/override_%d.json", TRACKS_DIR, builtin_idx);
}

bool session_store_save_builtin_override(int builtin_idx, const TrackDef *td)
{
    if (!sd_mgr_available() || !td) return false;
    if (builtin_idx < 0 || builtin_idx >= MAX_BUILTIN_TRACKS) return false;

    cJSON *obj = trackdef_to_json(td);
    if (!obj) return false;

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return false;

    char path[128];
    override_path(path, sizeof(path), builtin_idx);

    bool ok = sd_write_file(path, json, strlen(json));
    free(json);

    if (ok) {
        log_i("Saved builtin override %d", builtin_idx);
    }
    return ok;
}

void session_store_load_builtin_overrides()
{
    if (!sd_mgr_available()) return;

    char *buf = (char *)malloc(READ_BUF_SIZE);
    if (!buf) return;

    int loaded = 0;
    for (int i = 0; i < TRACK_DB_BUILTIN_COUNT && i < MAX_BUILTIN_TRACKS; i++) {
        char path[128];
        override_path(path, sizeof(path), i);

        g_builtin_override_set[i] = false;
        if (!sd_file_exists(path)) continue;
        if (!sd_read_file(path, buf, READ_BUF_SIZE)) continue;

        cJSON *root = cJSON_Parse(buf);
        if (!root) continue;

        if (json_to_trackdef(root, &g_builtin_overrides[i])) {
            g_builtin_override_set[i] = true;
            loaded++;
        }
        cJSON_Delete(root);
    }

    free(buf);
    if (loaded > 0) {
        log_i("Loaded %d builtin track overrides", loaded);
    }
}
