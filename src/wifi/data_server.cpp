/**
 * data_server.cpp — HTTP session download server
 */

#include "data_server.h"
#include "../storage/sd_mgr.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <WebServer.h>
#include <SD_MMC.h>

static WebServer s_server(80);
static bool      s_running = false;

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
static void handle_root() {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"device\":\"BRL-Laptimer\","
             "\"wifi_mode\":%d,"  // BRL_WIFI_OFF=0 AP=1 STA=2 OTA=3
             "\"sd\":%s,"
             "\"version\":\"1.0.0\"}",
             (int)g_state.wifi_mode,
             g_state.sd_available ? "true" : "false");
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
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String name = f.name();
            if (name.endsWith(".json")) {
                if (!first) json += ",";
                // Strip path and extension
                int slash = name.lastIndexOf('/');
                String id = (slash >= 0) ? name.substring(slash + 1) : name;
                id = id.substring(0, id.length() - 5);
                json += "\"" + id + "\"";
                first = false;
            }
        }
        f = dir.openNextFile();
    }
    json += "]";
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

static void handle_not_found() {
    s_server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void data_server_start() {
    if (s_running) return;

    s_server.on("/",                       HTTP_GET,    handle_root);
    s_server.on("/sessions",               HTTP_GET,    handle_sessions);
    s_server.on("/session/{}", HTTP_GET,    handle_session_get);
    s_server.on("/session/{}", HTTP_DELETE, handle_session_delete);
    s_server.onNotFound(handle_not_found);

    s_server.begin();
    s_running = true;
    Serial.println("[HTTP] Server started on port 80");
}

void data_server_stop() {
    if (!s_running) return;
    s_server.stop();
    s_running = false;
    Serial.println("[HTTP] Server stopped");
}

bool data_server_running() { return s_running; }

void data_server_poll() {
    if (s_running) s_server.handleClient();
}
