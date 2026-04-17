/**
 * session_store.cpp -- Session & track persistence (ESP-IDF / cJSON / VFS)
 *
 * Ported from ArduinoJson + SD_MMC to cJSON + POSIX VFS (via sd_mgr).
 *
 * File layout on SD:
 *   /sessions/<session_id>.json
 *   /tracks/<SafeName>.json          (user-created tracks)
 *   /tracks/builtin_NN.json          (coordinate overrides for built-in tracks)
 *
 * JSON format (example):
 * {
 *   "id": "20240315_143022",
 *   "name": "BRL_Timing_15.03_14:30",
 *   "track": "Nuerburgring GP",
 *   "laps": [
 *     { "lap": 1, "total_ms": 125432, "sectors": [41200, 42100, 42132],
 *       "track_points": [ [lat, lon, ms], ... ] },
 *     ...
 *   ]
 * }
 */

#include "session_store.h"
#include "sd_mgr.h"
#include "compat.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include "../gps/gps.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include "../timing/lap_timer.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "session_store";

static char s_session_path[64] = {};
static char s_track_name[48]   = {};
static char s_session_name[64] = {};

// ---------------------------------------------------------------------------
// GPS-based default session name: "BRL_Timing_DD.MM_HH:MM"
// Fallback if GPS time not yet valid: "BRL_Timing_<seconds>"
// ---------------------------------------------------------------------------
void session_store_make_default_name(char *buf, size_t len)
{
    GpsDateTime dt = gps_get_datetime();
    if (dt.valid) {
        snprintf(buf, len, "BRL_Timing_%02u.%02u_%02u:%02u",
                 dt.day, dt.month, dt.hour, dt.minute);
    } else {
        snprintf(buf, len, "BRL_Timing_%lu", (unsigned long)(millis() / 1000));
    }
}

// File ID: "YYYYMMDD_HHMMSS" from GPS, or "sess_XXXXXXXX" from millis
static void make_file_id(char *buf, size_t len)
{
    GpsDateTime dt = gps_get_datetime();
    if (dt.valid) {
        snprintf(buf, len, "%04u%02u%02u_%02u%02u%02u",
                 dt.year, dt.month, dt.day,
                 dt.hour, dt.minute, dt.second);
    } else {
        snprintf(buf, len, "sess_%08lX", (unsigned long)millis());
    }
}

// ---------------------------------------------------------------------------
// session_store_begin
// ---------------------------------------------------------------------------
void session_store_begin(const char *track_name, const char *session_name)
{
    strncpy(s_track_name,   track_name   ? track_name   : "", sizeof(s_track_name)   - 1);
    s_track_name[sizeof(s_track_name) - 1] = '\0';
    strncpy(s_session_name, session_name ? session_name : "", sizeof(s_session_name) - 1);
    s_session_name[sizeof(s_session_name) - 1] = '\0';

    char id[32];
    make_file_id(id, sizeof(id));
    strncpy(g_state.session.session_id, id, sizeof(g_state.session.session_id));
    g_state.session.session_id[sizeof(g_state.session.session_id) - 1] = '\0';

    if (!sd_file_exists("/sessions")) sd_make_dir("/sessions");
    snprintf(s_session_path, sizeof(s_session_path), "/sessions/%s.json", id);

    // Reset lap data for the new session
    lap_timer_reset_session();

    // Write initial JSON skeleton
    if (!g_state.sd_available) return;

    cJSON *doc = cJSON_CreateObject();
    if (!doc) { log_e("cJSON alloc failed"); return; }

    cJSON_AddStringToObject(doc, "id",    id);
    cJSON_AddStringToObject(doc, "name",  s_session_name);
    cJSON_AddStringToObject(doc, "track", s_track_name);
    cJSON_AddItemToObject(doc, "laps", cJSON_CreateArray());

    char *str = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);

    if (str) {
        sd_write_file(s_session_path, str, strlen(str));
        free(str);
    }

    log_i("Session begun: '%s'  file:%s", s_session_name, s_session_path);
}

// ---------------------------------------------------------------------------
// session_store_save_lap
// ---------------------------------------------------------------------------
void session_store_save_lap(uint8_t lap_idx, const char *video_stem)
{
    if (!g_state.sd_available) { log_w("save_lap %d: SD unavailable", lap_idx + 1); return; }

    // Defensive bootstrap: if begin() wasn't called (e.g. driver crossed the
    // start line before the session-name dialog auto-confirmed), create a
    // minimal session skeleton inline so the lap data is still persisted.
    // We must NOT call session_store_begin() here — it calls
    // lap_timer_reset_session() which would wipe the very lap we are about
    // to save.
    if (strlen(s_session_path) == 0) {
        log_w("save_lap %d: session_begin not called — auto-bootstrapping", lap_idx + 1);
        char id[32];
        make_file_id(id, sizeof(id));
        strncpy(g_state.session.session_id, id, sizeof(g_state.session.session_id) - 1);
        g_state.session.session_id[sizeof(g_state.session.session_id) - 1] = '\0';
        if (!sd_file_exists("/sessions")) sd_make_dir("/sessions");
        snprintf(s_session_path, sizeof(s_session_path), "/sessions/%s.json", id);
        strncpy(s_session_name, "Auto", sizeof(s_session_name) - 1);
        s_session_name[sizeof(s_session_name) - 1] = '\0';

        cJSON *doc = cJSON_CreateObject();
        if (doc) {
            cJSON_AddStringToObject(doc, "id",    id);
            cJSON_AddStringToObject(doc, "name",  s_session_name);
            cJSON_AddStringToObject(doc, "track", s_track_name);
            cJSON_AddItemToObject(doc, "laps", cJSON_CreateArray());
            char *str = cJSON_PrintUnformatted(doc);
            cJSON_Delete(doc);
            if (str) { sd_write_file(s_session_path, str, strlen(str)); free(str); }
        }
    }

    LapSession &sess = g_state.session;
    if (lap_idx >= sess.lap_count) { log_w("save_lap %d: idx >= lap_count(%u)", lap_idx + 1, sess.lap_count); return; }
    RecordedLap &rl = sess.laps[lap_idx];
    if (!rl.valid) { log_w("save_lap %d: lap not valid", lap_idx + 1); return; }

    // Working buffer in PSRAM — must be large enough for the entire growing
    // JSON. Each lap's track_points (~90 pts × ~50 B JSON = 4.5 KB) means
    // 8 KB overflows after 2 laps. 64 KB headroom = ~12 laps with full track.
    const size_t WORK_BUF_SIZE = 64 * 1024;
    char *work_buf = (char *)heap_caps_malloc(WORK_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!work_buf) { log_e("save_lap %d: PSRAM alloc failed", lap_idx + 1); return; }

    if (!sd_read_file(s_session_path, work_buf, WORK_BUF_SIZE)) {
        log_e("save_lap %d: read %s failed", lap_idx + 1, s_session_path);
        heap_caps_free(work_buf);
        return;
    }
    // Detect silent truncation: if read filled to capacity-1, file is too big.
    size_t read_len = strlen(work_buf);
    if (read_len >= WORK_BUF_SIZE - 1) {
        log_e("save_lap %d: session file > %u bytes — TRUNCATED, lap NOT saved",
              lap_idx + 1, (unsigned)WORK_BUF_SIZE);
        heap_caps_free(work_buf);
        return;
    }

    cJSON *doc = cJSON_Parse(work_buf);
    if (!doc) {
        log_e("save_lap %d: cJSON_Parse failed (file %u bytes)", lap_idx + 1, (unsigned)read_len);
        heap_caps_free(work_buf);
        return;
    }

    cJSON *laps = cJSON_GetObjectItem(doc, "laps");
    if (!laps) {
        laps = cJSON_CreateArray();
        cJSON_AddItemToObject(doc, "laps", laps);
    }

    cJSON *lap_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(lap_obj, "lap", lap_idx + 1);
    cJSON_AddNumberToObject(lap_obj, "total_ms", rl.total_ms);

    // Sectors
    cJSON *sectors = cJSON_CreateArray();
    for (uint8_t i = 0; i <= rl.sectors_used && i < MAX_SECTORS; i++) {
        cJSON_AddItemToArray(sectors, cJSON_CreateNumber(rl.sector_ms[i]));
    }
    cJSON_AddItemToObject(lap_obj, "sectors", sectors);

    // Track points: compact array of [lat, lon, ms]
    if (rl.points && rl.point_count > 0) {
        cJSON *pts = cJSON_CreateArray();
        // Limit to every 5th point to save space (~2 Hz)
        for (uint16_t i = 0; i < rl.point_count; i += 5) {
            cJSON *pt = cJSON_CreateArray();
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(rl.points[i].lat));
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(rl.points[i].lon));
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(rl.points[i].lap_ms));
            cJSON_AddItemToArray(pts, pt);
        }
        cJSON_AddItemToObject(lap_obj, "track_points", pts);
    }

    // Video file stem — app retrieves the lap video via GET /video/<stem>
    if (video_stem && video_stem[0]) {
        cJSON_AddStringToObject(lap_obj, "video", video_stem);
    }

    cJSON_AddItemToArray(laps, lap_obj);

    char *str = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    heap_caps_free(work_buf);

    if (!str) { log_e("save_lap %d: cJSON_Print failed", lap_idx + 1); return; }

    size_t out_len = strlen(str);
    bool ok = sd_write_file(s_session_path, str, out_len);
    free(str);

    if (!ok) {
        log_e("save_lap %d: sd_write_file %s failed (%u bytes)",
              lap_idx + 1, s_session_path, (unsigned)out_len);
        return;
    }
    log_i("Lap %d saved (%u bytes total)", lap_idx + 1, (unsigned)out_len);
}

// ---------------------------------------------------------------------------
// session_store_list_summaries
// ---------------------------------------------------------------------------
int session_store_list_summaries(SessionSummary *out, int max_count)
{
    if (!g_state.sd_available || !out || max_count <= 0) return 0;

    // opendir needs the full VFS path
    DIR *dir = opendir("/sdcard/sessions");
    if (!dir) return 0;

    // Working buffer in PSRAM — must match save_lap's WORK_BUF_SIZE so we can
    // read back files of the size we wrote. 8 KB silently truncated big files
    // and made cJSON_Parse fail, leaving lap_count = 0 in the summary list.
    const size_t WORK_BUF_SIZE = 64 * 1024;
    char *buf = (char *)heap_caps_malloc(WORK_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) { log_e("list_summaries: PSRAM alloc failed"); closedir(dir); return 0; }

    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && count < max_count) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6) continue;
        if (strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        // Build path for sd_read_file (without /sdcard prefix -- sd_mgr prepends it)
        char fpath[64];
        snprintf(fpath, sizeof(fpath), "/sessions/%s", ent->d_name);

        SessionSummary &s = out[count];
        memset(&s, 0, sizeof(s));

        // Extract ID from filename (strip .json)
        size_t id_len = nlen - 5;
        if (id_len >= sizeof(s.id)) id_len = sizeof(s.id) - 1;
        memcpy(s.id, ent->d_name, id_len);
        s.id[id_len] = '\0';

        if (!sd_read_file(fpath, buf, WORK_BUF_SIZE)) {
            log_w("list_summaries: read %s failed", fpath);
            strncpy(s.name, s.id, sizeof(s.name) - 1);
            count++;
            continue;
        }
        size_t read_len = strlen(buf);
        if (read_len >= WORK_BUF_SIZE - 1) {
            log_e("list_summaries: %s > %u bytes — truncated, lap_count will be wrong",
                  fpath, (unsigned)WORK_BUF_SIZE);
        }

        cJSON *doc = cJSON_Parse(buf);
        if (!doc) {
            log_e("list_summaries: cJSON_Parse failed for %s (%u bytes)",
                  fpath, (unsigned)read_len);
            strncpy(s.name, s.id, sizeof(s.name) - 1);
            count++;
            continue;
        }

        // name
        cJSON *j_name = cJSON_GetObjectItem(doc, "name");
        const char *n = (cJSON_IsString(j_name)) ? j_name->valuestring : "";
        strncpy(s.name, (strlen(n) > 0) ? n : s.id, sizeof(s.name) - 1);

        // track
        cJSON *j_track = cJSON_GetObjectItem(doc, "track");
        if (cJSON_IsString(j_track))
            strncpy(s.track, j_track->valuestring, sizeof(s.track) - 1);

        // laps: count and find best time
        cJSON *j_laps = cJSON_GetObjectItem(doc, "laps");
        s.lap_count = 0;
        uint32_t best = 0;
        if (cJSON_IsArray(j_laps)) {
            cJSON *lap_item;
            cJSON_ArrayForEach(lap_item, j_laps) {
                s.lap_count++;
                cJSON *j_ms = cJSON_GetObjectItem(lap_item, "total_ms");
                if (cJSON_IsNumber(j_ms)) {
                    uint32_t t = (uint32_t)j_ms->valuedouble;
                    if (t > 0 && (best == 0 || t < best)) best = t;
                }
            }
        }
        s.best_ms = best;

        cJSON_Delete(doc);
        count++;
    }

    closedir(dir);
    heap_caps_free(buf);
    return count;
}

// ---------------------------------------------------------------------------
// session_store_delete_session
// ---------------------------------------------------------------------------
bool session_store_delete_session(const char *session_id)
{
    if (!g_state.sd_available || !session_id || strlen(session_id) == 0) return false;

    char fpath[64];
    snprintf(fpath, sizeof(fpath), "/sessions/%s.json", session_id);

    if (!sd_file_exists(fpath)) {
        log_e("Session file not found: %s", fpath);
        return false;
    }

    bool ok = sd_delete_file(fpath);
    if (ok) log_i("Session deleted: %s", fpath);
    return ok;
}

// ---------------------------------------------------------------------------
// session_store_list
// ---------------------------------------------------------------------------
int session_store_list(char ids[][20], int max_count)
{
    if (!g_state.sd_available) return 0;

    DIR *dir = opendir("/sdcard/sessions");
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && count < max_count) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6) continue;
        if (strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        size_t id_len = nlen - 5;
        if (id_len > 19) id_len = 19;
        memcpy(ids[count], ent->d_name, id_len);
        ids[count][id_len] = '\0';
        count++;
    }

    closedir(dir);
    return count;
}

// ---------------------------------------------------------------------------
// User track persistence
// ---------------------------------------------------------------------------
void session_store_load_user_tracks()
{
    if (!g_state.sd_available) return;

    DIR *dir = opendir("/sdcard/tracks");
    if (!dir) return;

    g_user_track_count = 0;
    static char buf[2048];
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && g_user_track_count < MAX_USER_TRACKS) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6) continue;

        // Skip builtin coordinate-override files (builtin_NN.json)
        if (strncmp(ent->d_name, "builtin_", 8) == 0) continue;

        // Must end with .json
        if (strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        char fpath[80];
        snprintf(fpath, sizeof(fpath), "/tracks/%s", ent->d_name);

        if (!sd_read_file(fpath, buf, sizeof(buf))) {
            log_e("Failed to read %s", fpath);
            continue;
        }

        cJSON *doc = cJSON_Parse(buf);
        if (!doc) {
            // Rename the corrupt file once so it's not retried every boot
            // and the error log stops repeating. Content stays on the HDD
            // for manual inspection / recovery.
            char broken[96];
            snprintf(broken, sizeof(broken), "%s.bad", fpath);
            if (!sd_file_exists(broken)) {
                // sd_rename would be nice, but we don't have it — copy+delete
                static char copybuf[2048];
                if (sd_read_file(fpath, copybuf, sizeof(copybuf))) {
                    (void)sd_write_file(broken, copybuf, strlen(copybuf));
                }
                sd_delete_file(fpath);
                log_w("Moved unreadable track %s to %s", fpath, broken);
            } else {
                log_w("Skipping corrupt track JSON: %s", fpath);
            }
            continue;
        }

        // Validate: must have a non-empty name field (builtin overrides don't)
        cJSON *j_name = cJSON_GetObjectItem(doc, "name");
        const char *tname = (cJSON_IsString(j_name)) ? j_name->valuestring : "";
        if (strlen(tname) == 0) {
            cJSON_Delete(doc);
            continue;
        }

        TrackDef &td = g_user_tracks[g_user_track_count];
        memset(&td, 0, sizeof(td));
        strncpy(td.name, tname, sizeof(td.name) - 1);

        cJSON *j;

        j = cJSON_GetObjectItem(doc, "country");
        if (cJSON_IsString(j)) strncpy(td.country, j->valuestring, sizeof(td.country) - 1);

        j = cJSON_GetObjectItem(doc, "length_km");
        if (cJSON_IsNumber(j)) td.length_km = (float)j->valuedouble;

        j = cJSON_GetObjectItem(doc, "is_circuit");
        td.is_circuit = cJSON_IsTrue(j);

        td.user_created = true;

        // Start/finish line
        cJSON *sf = cJSON_GetObjectItem(doc, "sf");
        if (cJSON_IsArray(sf) && cJSON_GetArraySize(sf) >= 4) {
            td.sf_lat1 = cJSON_GetArrayItem(sf, 0)->valuedouble;
            td.sf_lon1 = cJSON_GetArrayItem(sf, 1)->valuedouble;
            td.sf_lat2 = cJSON_GetArrayItem(sf, 2)->valuedouble;
            td.sf_lon2 = cJSON_GetArrayItem(sf, 3)->valuedouble;
        }

        // Finish line (A-B stage)
        if (!td.is_circuit) {
            cJSON *fin = cJSON_GetObjectItem(doc, "fin");
            if (cJSON_IsArray(fin) && cJSON_GetArraySize(fin) >= 4) {
                td.fin_lat1 = cJSON_GetArrayItem(fin, 0)->valuedouble;
                td.fin_lon1 = cJSON_GetArrayItem(fin, 1)->valuedouble;
                td.fin_lat2 = cJSON_GetArrayItem(fin, 2)->valuedouble;
                td.fin_lon2 = cJSON_GetArrayItem(fin, 3)->valuedouble;
            }
        }

        // Sectors — accepts two JSON shapes:
        //   single-point: { "lat": …, "lon": …, "name": "S1" }
        //   2-point:      { "lat1":…, "lon1":…, "lat2":…, "lon2":…, "name":"S1" }
        // Any field missing → 0, and lap_timer treats (lat2==0 && lon2==0)
        // as "fall back to X-shape around (lat,lon)".
        cJSON *secs = cJSON_GetObjectItem(doc, "sectors");
        td.sector_count = 0;
        if (cJSON_IsArray(secs)) {
            int n = cJSON_GetArraySize(secs);
            for (int i = 0; i < n && td.sector_count < MAX_SECTORS; i++) {
                cJSON *sp = cJSON_GetArrayItem(secs, i);
                if (!sp) continue;
                SectorLine &sl = td.sectors[td.sector_count];
                memset(&sl, 0, sizeof(sl));

                // Point 1: prefer explicit "lat1/lon1", else fall back to "lat/lon"
                j = cJSON_GetObjectItem(sp, "lat1");
                if (!cJSON_IsNumber(j)) j = cJSON_GetObjectItem(sp, "lat");
                if (cJSON_IsNumber(j)) sl.lat = j->valuedouble;

                j = cJSON_GetObjectItem(sp, "lon1");
                if (!cJSON_IsNumber(j)) j = cJSON_GetObjectItem(sp, "lon");
                if (cJSON_IsNumber(j)) sl.lon = j->valuedouble;

                // Point 2 (optional — only set for true 2-point lines)
                j = cJSON_GetObjectItem(sp, "lat2");
                if (cJSON_IsNumber(j)) sl.lat2 = j->valuedouble;
                j = cJSON_GetObjectItem(sp, "lon2");
                if (cJSON_IsNumber(j)) sl.lon2 = j->valuedouble;

                j = cJSON_GetObjectItem(sp, "name");
                if (cJSON_IsString(j))
                    strncpy(sl.name, j->valuestring, SECTOR_NAME_LEN - 1);

                td.sector_count++;
            }
        }

        g_user_track_count++;
        log_i("Loaded user track: %s  sf=[%.6f, %.6f -> %.6f, %.6f]",
              td.name, td.sf_lat1, td.sf_lon1, td.sf_lat2, td.sf_lon2);

        cJSON_Delete(doc);
    }

    closedir(dir);
    log_i("Total user tracks loaded: %d", g_user_track_count);
}

bool session_store_save_user_track(const TrackDef *td)
{
    if (!g_state.sd_available || !td) return false;
    if (!sd_file_exists("/tracks")) sd_make_dir("/tracks");

    // Sanitize name for filename
    char safe[48];
    strncpy(safe, td->name, sizeof(safe) - 1);
    safe[sizeof(safe) - 1] = '\0';
    for (char *p = safe; *p; p++) {
        if (*p == ' ' || *p == '/' || *p == '\\') *p = '_';
    }

    char fname[80];
    snprintf(fname, sizeof(fname), "/tracks/%s.json", safe);

    cJSON *doc = cJSON_CreateObject();
    if (!doc) return false;

    cJSON_AddStringToObject(doc, "name",    td->name);
    cJSON_AddStringToObject(doc, "country", td->country);
    cJSON_AddNumberToObject(doc, "length_km", td->length_km);
    cJSON_AddBoolToObject(doc, "is_circuit", td->is_circuit);

    // Start/finish line as array of 4 doubles
    cJSON *sf = cJSON_CreateArray();
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lat1));
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lon1));
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lat2));
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lon2));
    cJSON_AddItemToObject(doc, "sf", sf);

    if (!td->is_circuit) {
        cJSON *fin = cJSON_CreateArray();
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lat1));
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lon1));
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lat2));
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lon2));
        cJSON_AddItemToObject(doc, "fin", fin);
    }

    // Sectors — writes 2-point shape when lat2/lon2 are set, single-point
    // shape otherwise (kept for back-compat with older app/firmware).
    cJSON *secs = cJSON_CreateArray();
    for (uint8_t i = 0; i < td->sector_count; i++) {
        const SectorLine &sl = td->sectors[i];
        cJSON *sp = cJSON_CreateObject();
        bool two_point = (sl.lat2 != 0.0 || sl.lon2 != 0.0);
        if (two_point) {
            cJSON_AddNumberToObject(sp, "lat1", sl.lat);
            cJSON_AddNumberToObject(sp, "lon1", sl.lon);
            cJSON_AddNumberToObject(sp, "lat2", sl.lat2);
            cJSON_AddNumberToObject(sp, "lon2", sl.lon2);
        } else {
            cJSON_AddNumberToObject(sp, "lat", sl.lat);
            cJSON_AddNumberToObject(sp, "lon", sl.lon);
        }
        cJSON_AddStringToObject(sp, "name", sl.name);
        cJSON_AddItemToArray(secs, sp);
    }
    cJSON_AddItemToObject(doc, "sectors", secs);

    char *str = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!str) return false;

    bool ok = sd_write_file(fname, str, strlen(str));
    free(str);

    if (ok) log_i("User track saved: %s", fname);
    return ok;
}

bool session_store_delete_user_track(int u_slot)
{
    if (u_slot < 0 || u_slot >= g_user_track_count) return false;

    // Delete file from SD
    if (g_state.sd_available) {
        const TrackDef &td = g_user_tracks[u_slot];
        char safe[48];
        strncpy(safe, td.name, sizeof(safe) - 1);
        safe[sizeof(safe) - 1] = '\0';
        for (char *p = safe; *p; p++) {
            if (*p == ' ' || *p == '/' || *p == '\\') *p = '_';
        }

        char fname[80];
        snprintf(fname, sizeof(fname), "/tracks/%s.json", safe);
        if (sd_file_exists(fname)) sd_delete_file(fname);
        log_i("User track deleted: %s", fname);
    }

    // Compact array
    for (int i = u_slot; i < g_user_track_count - 1; i++)
        g_user_tracks[i] = g_user_tracks[i + 1];
    g_user_track_count--;
    memset(&g_user_tracks[g_user_track_count], 0, sizeof(TrackDef));
    return true;
}

// ---------------------------------------------------------------------------
// Built-in track coordinate overrides
// ---------------------------------------------------------------------------
bool session_store_save_builtin_override(int builtin_idx, const TrackDef *td)
{
    if (!g_state.sd_available || !td) return false;
    if (builtin_idx < 0 || builtin_idx >= MAX_BUILTIN_TRACKS) return false;
    if (!sd_file_exists("/tracks")) sd_make_dir("/tracks");

    char fname[48];
    snprintf(fname, sizeof(fname), "/tracks/builtin_%02d.json", builtin_idx);

    cJSON *doc = cJSON_CreateObject();
    if (!doc) return false;

    cJSON_AddNumberToObject(doc, "builtin_idx", builtin_idx);

    cJSON *sf = cJSON_CreateArray();
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lat1));
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lon1));
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lat2));
    cJSON_AddItemToArray(sf, cJSON_CreateNumber(td->sf_lon2));
    cJSON_AddItemToObject(doc, "sf", sf);

    if (!td->is_circuit) {
        cJSON *fin = cJSON_CreateArray();
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lat1));
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lon1));
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lat2));
        cJSON_AddItemToArray(fin, cJSON_CreateNumber(td->fin_lon2));
        cJSON_AddItemToObject(doc, "fin", fin);
    }

    cJSON *secs = cJSON_CreateArray();
    for (uint8_t i = 0; i < td->sector_count; i++) {
        cJSON *sp = cJSON_CreateObject();
        cJSON_AddNumberToObject(sp, "lat",  td->sectors[i].lat);
        cJSON_AddNumberToObject(sp, "lon",  td->sectors[i].lon);
        cJSON_AddStringToObject(sp, "name", td->sectors[i].name);
        cJSON_AddItemToArray(secs, sp);
    }
    cJSON_AddItemToObject(doc, "sectors", secs);

    char *str = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!str) return false;

    bool ok = sd_write_file(fname, str, strlen(str));
    free(str);

    if (ok) log_i("Builtin override saved: %s", fname);
    return ok;
}

void session_store_load_builtin_overrides()
{
    if (!g_state.sd_available) return;

    static char buf[1024];

    for (int i = 0; i < MAX_BUILTIN_TRACKS && i < TRACK_DB_BUILTIN_COUNT; i++) {
        char fname[48];
        snprintf(fname, sizeof(fname), "/tracks/builtin_%02d.json", i);
        if (!sd_file_exists(fname)) continue;
        if (!sd_read_file(fname, buf, sizeof(buf))) continue;

        cJSON *doc = cJSON_Parse(buf);
        if (!doc) continue;

        // Start from original built-in definition, then override coords
        g_builtin_overrides[i] = TRACK_DB[i];
        TrackDef &td = g_builtin_overrides[i];
        td.user_created = false;

        // Override start/finish
        cJSON *sf = cJSON_GetObjectItem(doc, "sf");
        if (cJSON_IsArray(sf) && cJSON_GetArraySize(sf) >= 4) {
            td.sf_lat1 = cJSON_GetArrayItem(sf, 0)->valuedouble;
            td.sf_lon1 = cJSON_GetArrayItem(sf, 1)->valuedouble;
            td.sf_lat2 = cJSON_GetArrayItem(sf, 2)->valuedouble;
            td.sf_lon2 = cJSON_GetArrayItem(sf, 3)->valuedouble;
        }

        // Override finish line
        if (!td.is_circuit) {
            cJSON *fin = cJSON_GetObjectItem(doc, "fin");
            if (cJSON_IsArray(fin) && cJSON_GetArraySize(fin) >= 4) {
                td.fin_lat1 = cJSON_GetArrayItem(fin, 0)->valuedouble;
                td.fin_lon1 = cJSON_GetArrayItem(fin, 1)->valuedouble;
                td.fin_lat2 = cJSON_GetArrayItem(fin, 2)->valuedouble;
                td.fin_lon2 = cJSON_GetArrayItem(fin, 3)->valuedouble;
            }
        }

        // Override sectors
        cJSON *secs = cJSON_GetObjectItem(doc, "sectors");
        if (cJSON_IsArray(secs)) {
            td.sector_count = 0;
            cJSON *j;
            int n = cJSON_GetArraySize(secs);
            for (int si = 0; si < n && td.sector_count < MAX_SECTORS; si++) {
                cJSON *sp = cJSON_GetArrayItem(secs, si);
                if (!sp) continue;

                j = cJSON_GetObjectItem(sp, "lat");
                if (cJSON_IsNumber(j)) td.sectors[td.sector_count].lat = j->valuedouble;

                j = cJSON_GetObjectItem(sp, "lon");
                if (cJSON_IsNumber(j)) td.sectors[td.sector_count].lon = j->valuedouble;

                j = cJSON_GetObjectItem(sp, "name");
                if (cJSON_IsString(j))
                    strncpy(td.sectors[td.sector_count].name, j->valuestring,
                            SECTOR_NAME_LEN - 1);

                td.sector_count++;
            }
        }

        g_builtin_override_set[i] = true;
        log_i("Builtin override loaded for idx %d", i);

        cJSON_Delete(doc);
    }
}
