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

#define OTA_PASS    "brl2024"      // OTA password

static Preferences s_prefs;
static char s_sta_ssid[64] = {};
static char s_sta_pass[64] = {};
static char s_ap_ssid[32]  = {};
static char s_ap_pass[64]  = {};

// ---------------------------------------------------------------------------
void wifi_mgr_init() {
    // Load saved credentials from NVS
    s_prefs.begin("wifi", true);
    s_prefs.getString("ssid",    s_sta_ssid, sizeof(s_sta_ssid));
    s_prefs.getString("pass",    s_sta_pass, sizeof(s_sta_pass));
    s_prefs.getString("ap_ssid", s_ap_ssid,  sizeof(s_ap_ssid));
    s_prefs.getString("ap_pass", s_ap_pass,  sizeof(s_ap_pass));
    s_prefs.end();
    if (strlen(s_ap_ssid) == 0) strncpy(s_ap_ssid, "BRL-Laptimer", sizeof(s_ap_ssid)-1);

    // Initialize the WiFi driver NOW, while the heap is still clean.
    // esp_wifi_init() needs ~80 KB of contiguous IRAM/DRAM.  If we defer
    // initialization until the user enables the AP (seconds later), LVGL and
    // NimBLE/BLE allocations have already fragmented the heap AND the BLE
    // controller has reduced the WiFi coexistence static-RX-buffer count to 1
    // (below the required minimum of 4), causing ESP_ERR_NO_MEM (0x101 = 257).
    // Calling WiFi.mode() here forces wifiLowLevelInit() → esp_wifi_init()
    // before any BLE init takes place (caller must invoke wifi_mgr_init()
    // before obd_bt_init() in setup()).
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);   // forces esp_wifi_init() immediately
    WiFi.disconnect(false); // stop any STA activity, keep driver alive
    g_state.wifi_mode = BRL_WIFI_OFF;
    Serial.println("[WIFI] Manager init (driver pre-initialized)");
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

    // Stop current services — do NOT call WiFi.mode(NULL), that deinitializes
    // the driver and subsequent esp_wifi_init() fails with ESP_ERR_NO_MEM once
    // heap is fragmented.
    ArduinoOTA.end();
    data_server_stop();
    WiFi.softAPdisconnect(false);
    WiFi.disconnect(false);
    delay(150);

    g_state.wifi_mode = mode;
    strncpy(g_state.wifi_ssid, "", sizeof(g_state.wifi_ssid));

    switch (mode) {

        case BRL_WIFI_OFF:
            // Driver stays alive — just no active connection/AP
            Serial.println("[WIFI] OFF");
            break;

        case BRL_WIFI_AP:
            // NOTE: WiFi.setSleep(false) is FORBIDDEN when BLE is also running —
            // esp_wifi_set_ps(WIFI_PS_NONE) causes abort() in wifi_set_ps_process.
            // Modem sleep (default) is required for WiFi+BT coexistence.
            WiFi.setTxPower(WIFI_POWER_19_5dBm);
            WiFi.mode(WIFI_MODE_AP);
            delay(200);
            {
                bool ok = WiFi.softAP(s_ap_ssid,
                                      strlen(s_ap_pass) ? s_ap_pass : nullptr,
                                      6,     // channel 6 — reliable, avoids crowded ch1/11
                                      false, // not hidden
                                      4);    // max 4 clients
                delay(100);
                Serial.printf("[WIFI] AP %s  SSID:%s  IP:%s\n",
                              ok ? "OK" : "FAILED", s_ap_ssid,
                              WiFi.softAPIP().toString().c_str());
            }
            strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
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

const char *wifi_ap_ssid() { return s_ap_ssid; }
const char *wifi_ap_ip()   { return "192.168.4.1"; }

void wifi_ap_set_config(const char *ssid, const char *pass) {
    if (ssid && strlen(ssid) > 0) strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid)-1);
    if (pass) strncpy(s_ap_pass, pass, sizeof(s_ap_pass)-1);
    s_prefs.begin("wifi", false);
    s_prefs.putString("ap_ssid", s_ap_ssid);
    s_prefs.putString("ap_pass",  s_ap_pass);
    s_prefs.end();
    // If AP is currently active, restart it immediately
    if (g_state.wifi_mode == BRL_WIFI_AP) {
        WiFi.softAP(s_ap_ssid, strlen(s_ap_pass) ? s_ap_pass : nullptr, 6, false, 4);
    }
    Serial.printf("[WIFI] AP config updated: SSID=%s pass=%s\n",
                  s_ap_ssid, strlen(s_ap_pass) ? "***" : "(open)");
}
const char *wifi_ap_pass() { return s_ap_pass; }
