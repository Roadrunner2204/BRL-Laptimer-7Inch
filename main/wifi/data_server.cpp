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
#include "../data/lap_data.h"
#include "compat.h"

#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "esp_http_server.h"
#include "esp_log.h"

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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
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
    set_cors_headers(req);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// GET / -- status JSON
static esp_err_t handle_root(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /");
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"device\":\"BRL-Laptimer\","
             "\"wifi_mode\":%d,"
             "\"sd\":%s,"
             "\"version\":\"2.0.0\"}",
             (int)g_state.wifi_mode,
             g_state.sd_available ? "true" : "false");

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, strlen(buf));
}

// GET /sessions -- list session IDs from /sdcard/sessions/
static esp_err_t handle_sessions(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    if (!g_state.sd_available) {
        const char *err = "{\"error\":\"SD not available\"}";
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, err, strlen(err));
    }

    DIR *dir = opendir("/sdcard/sessions");
    if (!dir) {
        return httpd_resp_send(req, "[]", 2);
    }

    // Build JSON array of session IDs
    // Use chunked encoding to avoid sizing the buffer upfront
    httpd_resp_send_chunk(req, "[", 1);

    struct dirent *ent;
    bool first = true;
    while ((ent = readdir(dir)) != nullptr) {
        // Skip directories and non-.json files
        if (ent->d_type == DT_DIR) continue;
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 6 || strcmp(name + nlen - 5, ".json") != 0) continue;

        // Strip .json extension to get session ID
        char id[64];
        size_t id_len = nlen - 5;
        if (id_len >= sizeof(id)) id_len = sizeof(id) - 1;
        memcpy(id, name, id_len);
        id[id_len] = '\0';

        char entry[80];
        int elen;
        if (first) {
            elen = snprintf(entry, sizeof(entry), "\"%s\"", id);
            first = false;
        } else {
            elen = snprintf(entry, sizeof(entry), ",\"%s\"", id);
        }
        httpd_resp_send_chunk(req, entry, elen);
    }
    closedir(dir);

    httpd_resp_send_chunk(req, "]", 1);
    // Finish chunked response
    httpd_resp_send_chunk(req, nullptr, 0);

    ESP_LOGI(TAG, "GET /sessions");
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
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;

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
