/**
 * http_server.c — HTTP file server for the cam module.
 *
 * Streams recorded videos + telemetry to the phone / Studio. The cam
 * doesn't proxy through the laptimer — the laptimer's data_server
 * issues a 302 Redirect to <our-ip>:80 so bytes flow directly here.
 */

#include "http_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "../recorder/recorder.h"
#include "../recorder/sd_mgr.h"
#include "../wifi_sta/wifi_sta.h"
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "http_server";

static httpd_handle_t s_httpd = NULL;

/* ── Helpers ─────────────────────────────────────────────────────────── */
static esp_err_t set_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Connection", "close");
    return ESP_OK;
}

/* Pull a session-id out of "/video/<id>" or "/telemetry/<id>" with the
 * same path-traversal guards as the laptimer's data_server. */
static const char *extract_id(const char *uri, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return NULL;
    const char *id = uri + plen;
    if (*id == '\0') return NULL;
    if (strchr(id, '/'))     return NULL;
    if (strchr(id, '\\'))    return NULL;
    if (id[0] == '.')        return NULL;
    return id;
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    set_cors(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return httpd_resp_send(req, buf, n);
}

/* Stream a file to the response, chunked. Used for both video.avi and
 * telemetry.ndjson — large files that won't fit in RAM. */
static esp_err_t stream_file(httpd_req_t *req, const char *path,
                             const char *content_type)
{
    FILE *f = fopen(path, "rb");
    if (!f) return send_err(req, "404 Not Found", "not found");

    set_cors(req);
    httpd_resp_set_type(req, content_type);

    /* 8 KB stack-allocated chunk — keeps httpd task stack happy and
     * matches the SDMMC sector cache nicely. */
    char buf[8192];
    size_t n;
    esp_err_t result = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            ESP_LOGW(TAG, "client aborted %s", path);
            result = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return result;
}

/* ── Handlers ────────────────────────────────────────────────────────── */
static esp_err_t handle_options(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"device\":\"BRL-Cam\",\"ip\":\"%s\",\"rec\":%s,"
        "\"sd_total\":%" PRIu64 ",\"sd_free\":%" PRIu64 "}",
        wifi_sta_get_ip(),
        recorder_is_active() ? "true" : "false",
        sd_mgr_total_bytes(), sd_mgr_free_bytes());
    return httpd_resp_send(req, buf, n);
}

static esp_err_t handle_videos_list(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_type(req, "application/json");

    uint32_t count = 0;
    const CamVideoListEntry *entries = recorder_get_video_index(&count);

    httpd_resp_send_chunk(req, "[", 1);
    char entry[256];
    for (uint32_t i = 0; i < count; i++) {
        const CamVideoListEntry *e = &entries[i];
        int n = snprintf(entry, sizeof(entry),
            "%s{\"id\":\"%s\",\"size_bytes\":%" PRIu64
            ",\"gps_utc_ms_start\":%" PRIu64 ",\"duration_ms\":%" PRIu32 "}",
            (i == 0) ? "" : ",",
            e->session_id, e->size_bytes,
            e->gps_utc_ms_start, e->duration_ms);
        if (n > 0) httpd_resp_send_chunk(req, entry, n);
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_video_get(httpd_req_t *req)
{
    const char *id = extract_id(req->uri, "/video/");
    if (!id) return send_err(req, "400 Bad Request", "bad id");
    char path[160];
    snprintf(path, sizeof(path), "/sdcard/sessions/%s/video.avi", id);
    ESP_LOGI(TAG, "GET %s", req->uri);
    return stream_file(req, path, "video/x-msvideo");
}

static esp_err_t handle_telemetry_get(httpd_req_t *req)
{
    const char *id = extract_id(req->uri, "/telemetry/");
    if (!id) return send_err(req, "400 Bad Request", "bad id");
    char path[160];
    snprintf(path, sizeof(path), "/sdcard/sessions/%s/telemetry.ndjson", id);
    ESP_LOGI(TAG, "GET %s", req->uri);
    return stream_file(req, path, "application/x-ndjson");
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */
static const httpd_uri_t s_uris[] = {
    { .uri = "/",                .method = HTTP_GET,    .handler = handle_root,           .user_ctx = NULL },
    { .uri = "/",                .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = NULL },
    { .uri = "/videos/list",     .method = HTTP_GET,    .handler = handle_videos_list,     .user_ctx = NULL },
    { .uri = "/videos/list",     .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = NULL },
    { .uri = "/video/*",         .method = HTTP_GET,    .handler = handle_video_get,       .user_ctx = NULL },
    { .uri = "/video/*",         .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = NULL },
    { .uri = "/telemetry/*",     .method = HTTP_GET,    .handler = handle_telemetry_get,   .user_ctx = NULL },
    { .uri = "/telemetry/*",     .method = HTTP_OPTIONS,.handler = handle_options,         .user_ctx = NULL },
};

void http_server_start(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = BRL_CAM_HTTP_PORT;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size       = 12288;
    /* Long timeouts for multi-MB AVI streams over WiFi — same reasoning
     * as the laptimer's data_server. */
    cfg.send_wait_timeout = 30;
    cfg.recv_wait_timeout = 15;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_httpd = NULL;
        return;
    }
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &s_uris[i]);
    }
    ESP_LOGI(TAG, "HTTP server up on port %d", BRL_CAM_HTTP_PORT);
}

void http_server_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
}

uint16_t http_server_port(void) { return BRL_CAM_HTTP_PORT; }
