/**
 * wifi_mgr.cpp — WiFi manager
 */

#include "wifi_mgr.h"
#include "data_server.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

#define AP_SSID     "BRL-Laptimer"
#define AP_PASS     ""             // open network
#define OTA_PASS    "brl2024"      // OTA password

static Preferences s_prefs;
static char s_sta_ssid[64] = {};
static char s_sta_pass[64] = {};

// ---------------------------------------------------------------------------
void wifi_mgr_init() {
    // Load saved STA credentials from NVS
    s_prefs.begin("wifi", true);
    s_prefs.getString("ssid", s_sta_ssid, sizeof(s_sta_ssid));
    s_prefs.getString("pass", s_sta_pass, sizeof(s_sta_pass));
    s_prefs.end();

    WiFi.mode(WIFI_MODE_NULL);
    g_state.wifi_mode = BRL_WIFI_OFF;
    Serial.println("[WIFI] Manager init");
}

// ---------------------------------------------------------------------------
void wifi_set_sta(const char *ssid, const char *password) {
    strncpy(s_sta_ssid, ssid,     sizeof(s_sta_ssid) - 1);
    strncpy(s_sta_pass, password, sizeof(s_sta_pass) - 1);
    s_prefs.begin("wifi", false);
    s_prefs.putString("ssid", s_sta_ssid);
    s_prefs.putString("pass", s_sta_pass);
    s_prefs.end();
}

// ---------------------------------------------------------------------------
void wifi_set_mode(WifiMode mode) {
    if (g_state.wifi_mode == mode) return;

    // Tear down previous mode
    ArduinoOTA.end();
    data_server_stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
    delay(100);

    g_state.wifi_mode = mode;
    strncpy(g_state.wifi_ssid, "", sizeof(g_state.wifi_ssid));

    switch (mode) {

        case BRL_WIFI_OFF:
            Serial.println("[WIFI] OFF");
            break;

        case BRL_WIFI_AP:
            WiFi.mode(WIFI_MODE_AP);
            WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
            strncpy(g_state.wifi_ssid, AP_SSID, sizeof(g_state.wifi_ssid));
            Serial.printf("[WIFI] AP started: %s (192.168.4.1)\n", AP_SSID);
            data_server_start();
            break;

        case BRL_WIFI_STA:
            if (strlen(s_sta_ssid) == 0) {
                Serial.println("[WIFI] No STA credentials");
                g_state.wifi_mode = BRL_WIFI_OFF;
                return;
            }
            WiFi.mode(WIFI_MODE_STA);
            WiFi.begin(s_sta_ssid, s_sta_pass);
            strncpy(g_state.wifi_ssid, s_sta_ssid, sizeof(g_state.wifi_ssid));
            Serial.printf("[WIFI] Connecting to %s...\n", s_sta_ssid);

            // Setup OTA
            ArduinoOTA.setPassword(OTA_PASS);
            ArduinoOTA.setHostname("BRL-Laptimer");
            ArduinoOTA.onStart([]() {
                g_state.wifi_mode = BRL_WIFI_OTA;
                Serial.println("[OTA] Start");
            });
            ArduinoOTA.onEnd([]() {
                Serial.println("[OTA] Done — rebooting");
            });
            ArduinoOTA.onError([](ota_error_t err) {
                Serial.printf("[OTA] Error %u\n", err);
                g_state.wifi_mode = BRL_WIFI_STA;
            });
            ArduinoOTA.begin();
            break;

        case BRL_WIFI_OTA:
            // Entered automatically by ArduinoOTA callback
            break;
    }
}

// ---------------------------------------------------------------------------
void wifi_mgr_poll() {
    WifiMode mode = g_state.wifi_mode;

    if (mode == BRL_WIFI_STA || mode == BRL_WIFI_OTA) {
        ArduinoOTA.handle();
    }

    if (mode == BRL_WIFI_STA) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !data_server_running()) {
            Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());
            data_server_start();
        }
    }
}

const char *wifi_ap_ssid() { return AP_SSID; }
const char *wifi_ap_ip()   { return "192.168.4.1"; }
