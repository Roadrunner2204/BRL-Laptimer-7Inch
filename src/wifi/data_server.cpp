/**
 * data_server.cpp — HTTP session download server + captive-portal DNS stub
 *
 * Android detects "no internet" when connected to a WiFi AP that has no
 * real internet access.  It then routes all traffic through mobile data,
 * making 192.168.4.1 (our AP address) unreachable from apps that haven't
 * explicitly bound to the WiFi interface.
 *
 * Fix: run a DNS server that returns 192.168.4.1 for every hostname.
 * Android's connectivity check (connectivitycheck.gstatic.com/generate_204)
 * then reaches our HTTP server, we respond with 204 → Android marks the
 * network as "has internet" → all traffic goes through WiFi.
 */

#include "data_server.h"
#include "../storage/sd_mgr.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD_MMC.h>

static WebServer s_server(80);
static DNSServer s_dns;
static bool      s_running = false;

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
// Add permissive CORS headers so the app can also be served from a WebView.
static void add_cors_headers() {
    s_server.sendHeader("Access-Control-Allow-Origin",  "*");
    s_server.sendHeader("Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
    s_server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    s_server.sendHeader("Connection", "close");
}

static void handle_options() {
    add_cors_headers();
    s_server.send(204);
}

static void handle_root() {
    log_e("[HTTP] GET /");
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"device\":\"BRL-Laptimer\","
             "\"wifi_mode\":%d,"  // BRL_WIFI_AP=1 STA=2 OTA=3
             "\"sd\":%s,"
             "\"version\":\"1.0.0\"}",
             (int)g_state.wifi_mode,
             g_state.sd_available ? "true" : "false");
    add_cors_headers();
    s_server.send(200, "application/json", buf);
}

static void handle_sessions() {
    if (!g_state.sd_available) {
        s_server.send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }

    // List files in /sessions directory
    File dir = SD_MMC.open("/sessions");
    if (!dir) {
        s_server.send(200, "application/json", "[]");
        return;
    }

    String json = "[";
    bool first = true;
    for (;;) {
        File f = dir.openNextFile();
        if (!f) break;
        bool is_dir = f.isDirectory();
        String name = f.name();
        f.close();
        if (is_dir || !name.endsWith(".json")) continue;
        if (!first) json += ",";
        int slash = name.lastIndexOf('/');
        String id = (slash >= 0) ? name.substring(slash + 1) : name;
        id = id.substring(0, id.length() - 5);
        json += "\"" + id + "\"";
        first = false;
    }
    dir.close();
    json += "]";
    log_e("[HTTP] GET /sessions → %s", json.c_str());
    add_cors_headers();
    s_server.send(200, "application/json", json);
}

static void handle_session_get() {
    String id = s_server.pathArg(0);
    if (id.length() == 0 || id.indexOf('/') >= 0) {
        s_server.send(400, "application/json", "{\"error\":\"bad id\"}");
        return;
    }
    String path = "/sessions/" + id + ".json";
    if (!SD_MMC.exists(path)) {
        s_server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    File f = SD_MMC.open(path);
    if (!f) {
        s_server.send(500, "application/json", "{\"error\":\"open failed\"}");
        return;
    }
    s_server.streamFile(f, "application/json");
    f.close();
}

static void handle_session_delete() {
    String id = s_server.pathArg(0);
    if (id.length() == 0 || id.indexOf('/') >= 0) {
        s_server.send(400, "application/json", "{\"error\":\"bad id\"}");
        return;
    }
    String path = "/sessions/" + id + ".json";
    if (!SD_MMC.exists(path)) {
        s_server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    SD_MMC.remove(path);
    s_server.send(200, "application/json", "{\"ok\":true}");
}

// Android / iOS captive-portal detection endpoints.
// Returning 204 makes the OS believe the network has internet access,
// so it stops routing traffic through mobile data.
static void handle_generate_204() {
    log_e("[HTTP] captive-portal check → 204");
    add_cors_headers();
    s_server.send(204);
}

static void handle_not_found() {
    // Catch-all: also answer captive-portal probes that hit any path.
    // Some Android versions probe arbitrary paths on the gateway.
    const String &uri = s_server.uri();
    if (uri.indexOf("generate_204") >= 0 ||
        uri.indexOf("hotspot-detect") >= 0 ||
        uri.indexOf("ncsi") >= 0 ||
        uri.indexOf("connectivity") >= 0) {
        log_e("[HTTP] captive-portal probe %s → 204", uri.c_str());
        add_cors_headers();
        s_server.send(204);
        return;
    }
    s_server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void data_server_start() {
    if (s_running) return;

    s_server.on("/",                    HTTP_GET,     handle_root);
    s_server.on("/",                    HTTP_OPTIONS, handle_options);
    s_server.on("/sessions",            HTTP_GET,     handle_sessions);
    s_server.on("/sessions",            HTTP_OPTIONS, handle_options);
    s_server.on("/session/{}",          HTTP_GET,     handle_session_get);
    s_server.on("/session/{}",          HTTP_DELETE,  handle_session_delete);
    s_server.on("/session/{}",          HTTP_OPTIONS, handle_options);
    // Android captive-portal detection (makes OS think WiFi has internet)
    s_server.on("/generate_204",        HTTP_GET,     handle_generate_204);
    s_server.on("/hotspot-detect.html", HTTP_GET,     handle_generate_204); // iOS
    s_server.on("/ncsi.txt",            HTTP_GET,     handle_generate_204); // Windows
    s_server.onNotFound(handle_not_found);

    s_server.begin();

    // DNS server: redirect ALL hostnames → 192.168.4.1 so Android's
    // connectivity check reaches our HTTP server instead of timing out.
    s_dns.setTTL(300);
    s_dns.start(53, "*", IPAddress(192, 168, 4, 1));

    s_running = true;
    log_e("[HTTP] Server started on port 80, DNS redirect active");
}

void data_server_stop() {
    if (!s_running) return;
    s_dns.stop();
    s_server.stop();
    s_running = false;
    log_e("[HTTP] Server stopped");
}

bool data_server_running() { return s_running; }

void data_server_poll() {
    if (!s_running) return;
    s_dns.processNextRequest();   // handle one DNS query per tick
    s_server.handleClient();      // handle one HTTP request per tick
}
