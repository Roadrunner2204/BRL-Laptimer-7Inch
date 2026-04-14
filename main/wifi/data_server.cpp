/**
 * data_server.cpp -- HTTP session download server + captive-portal DNS stub
 *
 * Replaces Arduino WebServer + DNSServer with ESP-IDF esp_http_server and
 * a lightweight UDP DNS responder.
 *
 * Android detects "no internet" when connected to a WiFi AP that has no
 * real internet access.  It then routes all traffic through mobile data,
 * making 192.168.4.1 unreachable from apps that haven't explicitly bound
 * to the WiFi interface.
 *
 * Fix: run a DNS server that returns 192.168.4.1 for every hostname.
 * Android's connectivity check (connectivitycheck.gstatic.com/generate_204)
 * then reaches our HTTP server, we respond with 204 -> Android marks the
 * network as "has internet" -> all traffic goes through WiFi.
 */

#include "data_server.h"
#include "../storage/sd_mgr.h"
#include "../storage/session_store.h"
#include "../data/lap_data.h"
#include "../data/track_db.h"
#include "compat.h"

#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "data_server";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static httpd_handle_t s_httpd     = nullptr;
static int            s_dns_sock  = -1;
static bool           s_running   = false;

// ---------------------------------------------------------------------------
// CORS helpers
// ---------------------------------------------------------------------------
static esp_err_t set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Connection", "close");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Extract session ID from URI like "/session/20240101_120000"
// Returns pointer into uri string past "/session/", or NULL on error.
// ---------------------------------------------------------------------------
static const char *extract_session_id(const char *uri)
{
    const char *prefix = "/session/";
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return nullptr;
    const char *id = uri + plen;
    if (strlen(id) == 0) return nullptr;
    // Safety: reject path traversal
    if (strchr(id, '/') || strchr(id, '.') == id) return nullptr;
    return id;
}

// ---------------------------------------------------------------------------
// HTTP Handlers
// ---------------------------------------------------------------------------

// OPTIONS (preflight for CORS)
static esp_err_t handle_options(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OPTIONS %s", req->uri);
    set_cors_headers(req);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// GET / -- status JSON
static esp_err_t handle_root(httpd_req_t *req)
{
    // Log User-Agent so we can distinguish the BRL app from Android system
    // captive-portal probes (Dalvik/okhttp vs expo/react-native-fetch).
    char ua[96];
    if (httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua)) != ESP_OK) {
        strcpy(ua, "(no UA)");
    }
    ESP_LOGI(TAG, "GET /   UA=%s", ua);

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
             "{\"device\":\"BRL-Laptimer\","
             "\"wifi_mode\":%d,"
             "\"sd\":%s,"
             "\"version\":\"2.0.0\"}",
             (int)g_state.wifi_mode,
             g_state.sd_available ? "true" : "false");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, buf, n);
    ESP_LOGI(TAG, "GET / done: %s (%d bytes)", esp_err_to_name(e), n);
    return e;
}

// GET /sessions -- list session SUMMARIES so the Android app can show
// name/track/lap-count/best-time BEFORE the user downloads the full session.
// Each entry: {"id","name","track","lap_count","best_ms"}. id is the
// filename stem (e.g. "20260412_131035") — Android parses date/time from it.
static esp_err_t handle_sessions(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /sessions (summaries)");
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (!g_state.sd_available) {
        const char *err = "{\"error\":\"HDD not available\"}";
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, err, strlen(err));
    }

    SessionSummary *summaries = (SessionSummary *)heap_caps_malloc(
        sizeof(SessionSummary) * MAX_LAPS_PER_SESSION, MALLOC_CAP_SPIRAM);
    if (!summaries) {
        ESP_LOGE(TAG, "summaries alloc failed");
        return httpd_resp_send(req, "[]", 2);
    }
    int n = session_store_list_summaries(summaries, MAX_LAPS_PER_SESSION);

    httpd_resp_send_chunk(req, "[", 1);
    char entry[256];
    for (int i = 0; i < n; i++) {
        const SessionSummary &s = summaries[i];
        // JSON-escape minimal: we control the strings, but track names could
        // contain quotes — be safe and skip entries with non-printable chars.
        int elen = snprintf(entry, sizeof(entry),
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"track\":\"%s\","
            "\"lap_count\":%u,\"best_ms\":%lu}",
            (i == 0) ? "" : ",",
            s.id, s.name, s.track,
            (unsigned)s.lap_count, (unsigned long)s.best_ms);
        if (elen > 0) httpd_resp_send_chunk(req, entry, elen);
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, nullptr, 0);

    heap_caps_free(summaries);
    ESP_LOGI(TAG, "GET /sessions -- %d summaries sent", n);
    return ESP_OK;
}

// GET /session/{id} -- serve session JSON file (chunked for large files)
static esp_err_t handle_session_get(httpd_req_t *req)
{
    const char *id = extract_session_id(req->uri);
    if (!id) {
        set_cors_headers(req);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        const char *err = "{\"error\":\"bad id\"}";
        return httpd_resp_send(req, err, strlen(err));
    }

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/sessions/%s.json", id);

    FILE *f = fopen(path, "r");
    if (!f) {
        set_cors_headers(req);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        const char *err = "{\"error\":\"not found\"}";
        return httpd_resp_send(req, err, strlen(err));
    }

    ESP_LOGI(TAG, "GET /session/%s -- streaming file", id);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    // Stream file in chunks
    char chunk[1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(f);
            ESP_LOGE(TAG, "Failed to send chunk for session %s", id);
            // Abort chunked response
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);

    // Finish chunked response
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// DELETE /session/{id} -- delete session file
static esp_err_t handle_session_delete(httpd_req_t *req)
{
    const char *id = extract_session_id(req->uri);
    if (!id) {
        set_cors_headers(req);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        const char *err = "{\"error\":\"bad id\"}";
        return httpd_resp_send(req, err, strlen(err));
    }

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/sessions/%s.json", id);

    struct stat st;
    if (stat(path, &st) != 0) {
        set_cors_headers(req);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        const char *err = "{\"error\":\"not found\"}";
        return httpd_resp_send(req, err, strlen(err));
    }

    unlink(path);
    ESP_LOGI(TAG, "DELETE /session/%s", id);

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    const char *ok = "{\"ok\":true}";
    return httpd_resp_send(req, ok, strlen(ok));
}

// GET /videos -- list recorded AVI files with size
static esp_err_t handle_videos(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /videos");
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (!g_state.sd_available) {
        return httpd_resp_send(req, "[]", 2);
    }
    DIR *dir = opendir("/sdcard/videos");
    if (!dir) {
        return httpd_resp_send(req, "[]", 2);
    }

    httpd_resp_send_chunk(req, "[", 1);
    struct dirent *ent;
    bool first = true;
    char entry[160];
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type == DT_DIR) continue;
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 5 || strcmp(name + nlen - 4, ".avi") != 0) continue;

        char path[128];
        snprintf(path, sizeof(path), "/sdcard/videos/%s", name);
        struct stat st;
        long size = 0;
        if (stat(path, &st) == 0) size = (long)st.st_size;

        // id = filename without .avi
        char id[80];
        size_t id_len = nlen - 4;
        if (id_len >= sizeof(id)) id_len = sizeof(id) - 1;
        memcpy(id, name, id_len);
        id[id_len] = '\0';

        int elen = snprintf(entry, sizeof(entry),
            "%s{\"id\":\"%s\",\"size\":%ld}",
            first ? "" : ",", id, size);
        if (elen > 0) httpd_resp_send_chunk(req, entry, elen);
        first = false;
    }
    closedir(dir);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// GET /video/{id} -- stream AVI file
static esp_err_t handle_video_get(httpd_req_t *req)
{
    // URI is /video/<id>
    const char *prefix = "/video/";
    size_t plen = strlen(prefix);
    if (strncmp(req->uri, prefix, plen) != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "bad uri", 7);
    }
    const char *id = req->uri + plen;
    if (strlen(id) == 0 || strchr(id, '/') || strchr(id, '.') == id) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "bad id", 6);
    }

    char path[160];
    snprintf(path, sizeof(path), "/sdcard/videos/%s.avi", id);

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_cors_headers(req);
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "{\"error\":\"not found\"}", 21);
    }

    ESP_LOGI(TAG, "GET /video/%s -- streaming", id);
    set_cors_headers(req);
    httpd_resp_set_type(req, "video/avi");

    // Stream in big chunks for throughput. 32 KB is a good balance for
    // WiFi RTT + SD read speed.
    static uint8_t chunk[32 * 1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)chunk, n) != ESP_OK) {
            fclose(f);
            ESP_LOGW(TAG, "/video/%s: client aborted", id);
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// DELETE /video/{id} -- remove AVI from SD
static esp_err_t handle_video_delete(httpd_req_t *req)
{
    const char *prefix = "/video/";
    size_t plen = strlen(prefix);
    if (strncmp(req->uri, prefix, plen) != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "bad uri", 7);
    }
    const char *id = req->uri + plen;
    if (strlen(id) == 0 || strchr(id, '/') || strchr(id, '.') == id) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "bad id", 6);
    }
    char path[160];
    snprintf(path, sizeof(path), "/sdcard/videos/%s.avi", id);
    struct stat st;
    if (stat(path, &st) != 0) {
        set_cors_headers(req);
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "{\"error\":\"not found\"}", 21);
    }
    unlink(path);
    ESP_LOGI(TAG, "DELETE /video/%s", id);
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

// ---------------------------------------------------------------------------
// Tracks API
//
// GET  /tracks        -- list all known tracks (builtin + user) as JSON
// POST /track         -- accept JSON body, persist to /sdcard/tracks/*.json,
//                        then reload the live user-track catalog so the new
//                        track is immediately usable on the device UI.
//
// JSON body format (same as the on-SD file so we can round-trip):
//   { "name":"...", "country":"DE", "length_km":4.5, "is_circuit":true,
//     "sf":[lat1,lon1,lat2,lon2],
//     "fin":[lat1,lon1,lat2,lon2],      // optional, A-B only
//     "sectors":[{"lat":..,"lon":..,"name":"S1"}, ...] }
// ---------------------------------------------------------------------------

// Forward decl — send_err is defined further down.
static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg);

// ---------------------------------------------------------------------------
// GET /track/<idx> — full TrackDef (sf, fin, sectors incl. 2-point lines)
// ---------------------------------------------------------------------------
static esp_err_t handle_track_get_one(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *slash = strrchr(uri, '/');
    if (!slash || !slash[1]) return send_err(req, "400 Bad Request", "missing idx");
    int idx = atoi(slash + 1);
    if (idx < 0 || idx >= track_total_count())
        return send_err(req, "404 Not Found", "idx out of range");

    const TrackDef *td = track_get(idx);
    if (!td) return send_err(req, "404 Not Found", "no such track");

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "index", idx);
    cJSON_AddStringToObject(doc, "name", td->name);
    cJSON_AddStringToObject(doc, "country", td->country);
    cJSON_AddNumberToObject(doc, "length_km", td->length_km);
    cJSON_AddBoolToObject(doc, "is_circuit", td->is_circuit);
    cJSON_AddBoolToObject(doc, "user_created", td->user_created);

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
    if (!str) return send_err(req, "500", "oom");
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, str);
    free(str);
    return r;
}

static esp_err_t handle_tracks_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /tracks");
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    httpd_resp_send_chunk(req, "[", 1);
    char entry[224];
    int total = track_total_count();
    bool first = true;
    for (int i = 0; i < total; i++) {
        const TrackDef *td = track_get(i);
        if (!td) continue;
        if (track_is_shadowed(i)) continue;   // hide dup-name bundle entries
        int elen = snprintf(entry, sizeof(entry),
            "%s{\"index\":%d,\"name\":\"%s\",\"country\":\"%s\","
            "\"is_circuit\":%s,\"user_created\":%s,"
            "\"sector_count\":%u,\"length_km\":%.3f}",
            first ? "" : ",", i,
            td->name, td->country,
            td->is_circuit ? "true" : "false",
            td->user_created ? "true" : "false",
            (unsigned)td->sector_count, td->length_km);
        if (elen > 0) {
            httpd_resp_send_chunk(req, entry, elen);
            first = false;
        }
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// Reply helper: {"error":"..."} with status + CORS
static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handle_track_post(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /track (len=%d)", (int)req->content_len);

    if (!g_state.sd_available) {
        return send_err(req, "503 Service Unavailable", "HDD not available");
    }
    if (req->content_len <= 0 || req->content_len > 8192) {
        return send_err(req, "413 Payload Too Large", "body 1..8192 bytes required");
    }

    // Read full body
    char *body = (char *)malloc(req->content_len + 1);
    if (!body) return send_err(req, "500 Internal Server Error", "oom");
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, body + total, req->content_len - total);
        if (r <= 0) {
            free(body);
            return send_err(req, "400 Bad Request", "recv failed");
        }
        total += r;
    }
    body[total] = '\0';

    cJSON *doc = cJSON_Parse(body);
    free(body);
    if (!doc) return send_err(req, "400 Bad Request", "invalid JSON");

    TrackDef td = {};
    td.user_created = true;

    cJSON *j = cJSON_GetObjectItem(doc, "name");
    if (!cJSON_IsString(j) || strlen(j->valuestring) < 2) {
        cJSON_Delete(doc);
        return send_err(req, "400 Bad Request", "name required (>=2 chars)");
    }
    strncpy(td.name, j->valuestring, sizeof(td.name) - 1);

    j = cJSON_GetObjectItem(doc, "country");
    if (cJSON_IsString(j)) strncpy(td.country, j->valuestring, sizeof(td.country) - 1);
    else strncpy(td.country, "Custom", sizeof(td.country) - 1);

    j = cJSON_GetObjectItem(doc, "length_km");
    if (cJSON_IsNumber(j)) td.length_km = (float)j->valuedouble;

    j = cJSON_GetObjectItem(doc, "is_circuit");
    td.is_circuit = cJSON_IsTrue(j);

    // Start/finish line — required, 4 numbers
    cJSON *sf = cJSON_GetObjectItem(doc, "sf");
    if (!cJSON_IsArray(sf) || cJSON_GetArraySize(sf) < 4) {
        cJSON_Delete(doc);
        return send_err(req, "400 Bad Request", "sf[4] required");
    }
    td.sf_lat1 = cJSON_GetArrayItem(sf, 0)->valuedouble;
    td.sf_lon1 = cJSON_GetArrayItem(sf, 1)->valuedouble;
    td.sf_lat2 = cJSON_GetArrayItem(sf, 2)->valuedouble;
    td.sf_lon2 = cJSON_GetArrayItem(sf, 3)->valuedouble;

    // Finish line — only required for A-B
    if (!td.is_circuit) {
        cJSON *fin = cJSON_GetObjectItem(doc, "fin");
        if (!cJSON_IsArray(fin) || cJSON_GetArraySize(fin) < 4) {
            cJSON_Delete(doc);
            return send_err(req, "400 Bad Request", "fin[4] required for A-B track");
        }
        td.fin_lat1 = cJSON_GetArrayItem(fin, 0)->valuedouble;
        td.fin_lon1 = cJSON_GetArrayItem(fin, 1)->valuedouble;
        td.fin_lat2 = cJSON_GetArrayItem(fin, 2)->valuedouble;
        td.fin_lon2 = cJSON_GetArrayItem(fin, 3)->valuedouble;
    }

    // Sectors (optional, up to MAX_SECTORS)
    // Accept single-point ({lat,lon,name}) and VBOX-style 2-point
    // ({lat1,lon1,lat2,lon2,name}). lap_timer picks X-shape or straight
    // line based on whether lat2/lon2 are non-zero.
    cJSON *secs = cJSON_GetObjectItem(doc, "sectors");
    if (cJSON_IsArray(secs)) {
        int n = cJSON_GetArraySize(secs);
        for (int i = 0; i < n && td.sector_count < MAX_SECTORS; i++) {
            cJSON *sp = cJSON_GetArrayItem(secs, i);
            if (!sp) continue;
            SectorLine &sl = td.sectors[td.sector_count];
            memset(&sl, 0, sizeof(sl));

            cJSON *lat1 = cJSON_GetObjectItem(sp, "lat1");
            if (!cJSON_IsNumber(lat1)) lat1 = cJSON_GetObjectItem(sp, "lat");
            cJSON *lon1 = cJSON_GetObjectItem(sp, "lon1");
            if (!cJSON_IsNumber(lon1)) lon1 = cJSON_GetObjectItem(sp, "lon");
            if (!cJSON_IsNumber(lat1) || !cJSON_IsNumber(lon1)) continue;
            sl.lat = lat1->valuedouble;
            sl.lon = lon1->valuedouble;

            cJSON *lat2 = cJSON_GetObjectItem(sp, "lat2");
            cJSON *lon2 = cJSON_GetObjectItem(sp, "lon2");
            if (cJSON_IsNumber(lat2)) sl.lat2 = lat2->valuedouble;
            if (cJSON_IsNumber(lon2)) sl.lon2 = lon2->valuedouble;

            cJSON *sname = cJSON_GetObjectItem(sp, "name");
            if (cJSON_IsString(sname))
                strncpy(sl.name, sname->valuestring, SECTOR_NAME_LEN - 1);
            else
                snprintf(sl.name, SECTOR_NAME_LEN, "S%d", td.sector_count + 1);

            td.sector_count++;
        }
    }

    cJSON_Delete(doc);

    // Hold g_state_mutex across save + catalog reload. Without this, the
    // HTTP task (Core 0) memsets g_user_tracks[] while the LVGL task
    // (Core 1) may be reading track_get(...)->name for label text — race
    // causes random LoadProhibited on the display.
    bool locked = (g_state_mutex &&
                   xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(2000)) == pdTRUE);

    // ---- Diagnostics: log received coords so we can verify roundtrip ----
    ESP_LOGI(TAG, "POST /track RX: name='%s' country='%s' is_circuit=%d "
                  "sf=[%.6f, %.6f -> %.6f, %.6f] sectors=%u",
             td.name, td.country, td.is_circuit ? 1 : 0,
             td.sf_lat1, td.sf_lon1, td.sf_lat2, td.sf_lon2,
             (unsigned)td.sector_count);

    bool saved = session_store_save_user_track(&td);
    if (saved) session_store_load_user_tracks();

    if (locked) xSemaphoreGive(g_state_mutex);

    if (!saved) {
        ESP_LOGE(TAG, "POST /track: save_user_track FAILED for '%s'", td.name);
        return send_err(req, "500 Internal Server Error", "save failed");
    }

    // Verify what's now in g_user_tracks for the same name
    bool found = false;
    for (int u = 0; u < g_user_track_count; u++) {
        if (strcmp(g_user_tracks[u].name, td.name) == 0) {
            const TrackDef &v = g_user_tracks[u];
            ESP_LOGI(TAG, "POST /track OK: g_user_tracks[%d] '%s' "
                          "sf=[%.6f, %.6f -> %.6f, %.6f]",
                     u, v.name,
                     v.sf_lat1, v.sf_lon1, v.sf_lat2, v.sf_lon2);
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "POST /track: '%s' was not in g_user_tracks after reload! "
                      "(count=%d)", td.name, g_user_track_count);
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char ok[128];
    int n = snprintf(ok, sizeof(ok),
                     "{\"ok\":true,\"name\":\"%s\",\"total\":%d}",
                     td.name, track_total_count());
    return httpd_resp_send(req, ok, n);
}

// GET /generate_204 -- Android captive portal detection
static esp_err_t handle_generate_204(httpd_req_t *req)
{
    ESP_LOGI(TAG, "captive-portal check -> 204");
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

// ---------------------------------------------------------------------------
// DNS captive portal responder
//
// Minimal UDP DNS server that responds to ALL queries with 192.168.4.1.
// This makes Android/iOS believe the network has internet so they stop
// routing traffic through mobile data.
// ---------------------------------------------------------------------------
static void dns_server_start(void)
{
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "DNS: socket() failed: errno %d", errno);
        return;
    }

    // Set non-blocking
    int flags = fcntl(s_dns_sock, F_GETFL, 0);
    fcntl(s_dns_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind() failed: errno %d", errno);
        close(s_dns_sock);
        s_dns_sock = -1;
        return;
    }

    ESP_LOGI(TAG, "DNS captive-portal responder started on port 53");
}

static void dns_server_stop(void)
{
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
}

/**
 * Process one DNS query (non-blocking).
 *
 * DNS response format (simplified):
 *   - Copy the query header, set QR=1 (response), ANCOUNT=1
 *   - Copy the question section as-is
 *   - Append one A record answer pointing to 192.168.4.1
 *
 * We use name compression (0xC00C) pointing to the name in the question.
 */
static void dns_process_one(void)
{
    if (s_dns_sock < 0) return;

    uint8_t buf[512];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    ssize_t n = recvfrom(s_dns_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client_addr, &addr_len);
    if (n <= 0) return;  // no data or error (EWOULDBLOCK)

    // Minimum DNS header is 12 bytes
    if (n < 12) return;

    // Build response in-place
    // Header: set QR=1 (response), AA=1, RA=1, RCODE=0
    buf[2] = 0x84;  // QR=1, AA=1
    buf[3] = 0x00;  // RA=0, RCODE=0
    // ANCOUNT = 1
    buf[6] = 0x00;
    buf[7] = 0x01;
    // NSCOUNT = 0, ARCOUNT = 0
    buf[8] = 0; buf[9] = 0;
    buf[10] = 0; buf[11] = 0;

    // Find end of question section (skip name + QTYPE + QCLASS)
    size_t pos = 12;
    while (pos < (size_t)n && buf[pos] != 0) {
        pos += 1 + buf[pos];  // label length + label
    }
    pos++;       // skip null terminator
    pos += 4;    // skip QTYPE (2) + QCLASS (2)

    if (pos > (size_t)n) return;  // malformed

    // Append answer: name pointer (0xC00C -> offset 12 = question name),
    // TYPE A (1), CLASS IN (1), TTL 300, RDLENGTH 4, RDATA 192.168.4.1
    uint8_t answer[] = {
        0xC0, 0x0C,             // name pointer to question
        0x00, 0x01,             // TYPE A
        0x00, 0x01,             // CLASS IN
        0x00, 0x00, 0x01, 0x2C, // TTL = 300
        0x00, 0x04,             // RDLENGTH = 4
        192, 168, 4, 1          // RDATA = 192.168.4.1
    };

    if (pos + sizeof(answer) > sizeof(buf)) return;  // overflow
    memcpy(buf + pos, answer, sizeof(answer));
    size_t resp_len = pos + sizeof(answer);

    sendto(s_dns_sock, buf, resp_len, 0,
           (struct sockaddr *)&client_addr, addr_len);
}

// ---------------------------------------------------------------------------
// URI handler registration table
// ---------------------------------------------------------------------------
static const httpd_uri_t s_uri_handlers[] = {
    { .uri = "/",              .method = HTTP_GET,    .handler = handle_root,           .user_ctx = nullptr },
    { .uri = "/",              .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/sessions",      .method = HTTP_GET,    .handler = handle_sessions,        .user_ctx = nullptr },
    { .uri = "/sessions",      .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/session/*",     .method = HTTP_GET,    .handler = handle_session_get,     .user_ctx = nullptr },
    { .uri = "/session/*",     .method = HTTP_DELETE, .handler = handle_session_delete,  .user_ctx = nullptr },
    { .uri = "/session/*",     .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/videos",        .method = HTTP_GET,    .handler = handle_videos,          .user_ctx = nullptr },
    { .uri = "/videos",        .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/video/*",       .method = HTTP_GET,    .handler = handle_video_get,       .user_ctx = nullptr },
    { .uri = "/video/*",       .method = HTTP_DELETE, .handler = handle_video_delete,    .user_ctx = nullptr },
    { .uri = "/video/*",       .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/tracks",        .method = HTTP_GET,    .handler = handle_tracks_get,      .user_ctx = nullptr },
    { .uri = "/tracks",        .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/track",         .method = HTTP_POST,   .handler = handle_track_post,      .user_ctx = nullptr },
    { .uri = "/track",         .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/track/*",       .method = HTTP_GET,    .handler = handle_track_get_one,   .user_ctx = nullptr },
    { .uri = "/track/*",       .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = nullptr },
    { .uri = "/generate_204",  .method = HTTP_GET,    .handler = handle_generate_204,    .user_ctx = nullptr },
    { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_generate_204, .user_ctx = nullptr },
    { .uri = "/ncsi.txt",      .method = HTTP_GET,    .handler = handle_generate_204,    .user_ctx = nullptr },
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void data_server_start(void)
{
    if (s_running) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn   = httpd_uri_match_wildcard;
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;
    // Default stack is 4 KB — too small for POST /track, which allocates a
    // TrackDef on stack, parses cJSON, writes SD, then reloads the whole
    // user-track catalog (N × cJSON_Parse + 2 KB static buf). Overflows
    // the httpd task and panics. Bump to 12 KB.
    config.stack_size = 12288;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return;
    }

    // Register all URI handlers
    for (size_t i = 0; i < sizeof(s_uri_handlers) / sizeof(s_uri_handlers[0]); i++) {
        httpd_register_uri_handler(s_httpd, &s_uri_handlers[i]);
    }

    // Start DNS captive-portal responder
    dns_server_start();

    s_running = true;
    ESP_LOGI(TAG, "HTTP server started on port 80, DNS redirect active");
}

void data_server_stop(void)
{
    if (!s_running) return;

    dns_server_stop();

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }

    s_running = false;
    ESP_LOGI(TAG, "HTTP server stopped");
}

bool data_server_running(void)
{
    return s_running;
}

void data_server_poll(void)
{
    if (!s_running) return;
    // Process one DNS query per tick (non-blocking)
    dns_process_one();
    // esp_http_server runs in its own task, no manual polling needed
}
