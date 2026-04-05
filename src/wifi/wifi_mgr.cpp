/**
 * wifi_mgr.cpp — WiFi manager
 *
 * IMPORTANT — WiFi + BLE coexistence rule (ESP-IDF hard requirement):
 *   WIFI_PS_NONE is FORBIDDEN when BLE is active. Any call to
 *   esp_wifi_set_ps(WIFI_PS_NONE) / WiFi.setSleep(false) while NimBLE is
 *   running causes abort() in pm_set_sleep_type().
 *   WiFi.mode() itself may reset PS to NONE internally, so we call
 *   WiFi.setSleep(true) after EVERY mode change.
 */

#include "wifi_mgr.h"
#include "data_server.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

#define OTA_PASS    "brl2024"

static Preferences s_prefs;
static char s_sta_ssid[64] = {};
static char s_sta_pass[64] = {};
static char s_ap_ssid[32]  = {};
static char s_ap_pass[64]  = {};

// Force modem sleep after every WiFi.mode() call — required when BLE runs.
static inline void wifi_ensure_sleep() {
    WiFi.setSleep(true);   // WIFI_PS_MIN_MODEM — compatible with BLE coex
}

// ---------------------------------------------------------------------------
void wifi_mgr_init() {
    s_prefs.begin("wifi", true);
    s_prefs.getString("ssid",    s_sta_ssid, sizeof(s_sta_ssid));
    s_prefs.getString("pass",    s_sta_pass, sizeof(s_sta_pass));
    s_prefs.getString("ap_ssid", s_ap_ssid,  sizeof(s_ap_ssid));
    s_prefs.getString("ap_pass", s_ap_pass,  sizeof(s_ap_pass));
    s_prefs.end();
    if (strlen(s_ap_ssid) == 0) strncpy(s_ap_ssid, "BRL-Laptimer", sizeof(s_ap_ssid)-1);

    // Force esp_wifi_init() NOW, before NimBLE/BLE starts.
    // BLE controller init reduces WiFi coex static-RX-buffer count below the
    // required minimum of 4 → esp_wifi_init() fails with ESP_ERR_NO_MEM later.
    // Caller (main.cpp) must call wifi_mgr_init() BEFORE obd_bt_init().
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);     // triggers wifiLowLevelInit() → esp_wifi_init()
    wifi_ensure_sleep();     // prevent PS=NONE crash when BLE starts later
    WiFi.disconnect(false);
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

    ArduinoOTA.end();
    data_server_stop();
    WiFi.softAPdisconnect(false);
    WiFi.disconnect(false);
    delay(150);

    g_state.wifi_mode = mode;
    strncpy(g_state.wifi_ssid, "", sizeof(g_state.wifi_ssid));

    switch (mode) {

        case BRL_WIFI_OFF:
            Serial.println("[WIFI] OFF");
            break;

        case BRL_WIFI_AP:
            WiFi.setTxPower(WIFI_POWER_19_5dBm);
            WiFi.mode(WIFI_MODE_AP);
            wifi_ensure_sleep();   // WiFi.mode() resets PS to NONE — restore it
            delay(200);
            {
                bool ok = WiFi.softAP(s_ap_ssid,
                                      strlen(s_ap_pass) ? s_ap_pass : nullptr,
                                      6, false, 4);
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
            wifi_ensure_sleep();   // WiFi.mode() resets PS to NONE — restore it
            WiFi.begin(s_sta_ssid, s_sta_pass);
            strncpy(g_state.wifi_ssid, s_sta_ssid, sizeof(g_state.wifi_ssid));
            Serial.printf("[WIFI] Connecting to %s...\n", s_sta_ssid);

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
    if (g_state.wifi_mode == BRL_WIFI_AP) {
        WiFi.softAP(s_ap_ssid, strlen(s_ap_pass) ? s_ap_pass : nullptr, 6, false, 4);
    }
    Serial.printf("[WIFI] AP config updated: SSID=%s pass=%s\n",
                  s_ap_ssid, strlen(s_ap_pass) ? "***" : "(open)");
}
const char *wifi_ap_pass() { return s_ap_pass; }
