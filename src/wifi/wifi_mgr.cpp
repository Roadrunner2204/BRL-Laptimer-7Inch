/**
 * wifi_mgr.cpp — WiFi manager
 *
 * Strategy: initialize WIFI_MODE_APSTA at boot (pre-allocates both STA and AP
 * interface objects while heap is clean).  At runtime we never call WiFi.mode()
 * again — we just activate/deactivate via softAP() / WiFi.begin().
 *
 * Why:
 *  - Switching modes at runtime (WIFI_MODE_NULL → WIFI_MODE_AP) requires
 *    esp_wifi_init() + interface allocation.  By then, LVGL/BLE allocations
 *    have fragmented the heap → LoadProhibited crash in ieee80211_hostap_attach
 *    (null VAP pointer from failed alloc) or ESP_ERR_NO_MEM in esp_wifi_init.
 *  - WiFi.mode() internally resets PS to WIFI_PS_NONE.  With NimBLE/BLE active
 *    this triggers abort() in pm_set_sleep_type ("enable WiFi modem sleep!").
 *  - Keeping WiFi in APSTA avoids both problems: all memory is pre-allocated,
 *    and we call WiFi.setSleep(true) once after the initial mode() call.
 *
 * Caller must invoke wifi_mgr_init() BEFORE obd_bt_init() in setup().
 */

#include "wifi_mgr.h"
#include "data_server.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Preferences.h>

#define OTA_PASS  "brl2024"

static Preferences s_prefs;
static char s_sta_ssid[64] = {};
static char s_sta_pass[64] = {};
static char s_ap_ssid[32]  = {};
static char s_ap_pass[64]  = {};

// ---------------------------------------------------------------------------
void wifi_mgr_init() {
    s_prefs.begin("wifi", true);
    s_prefs.getString("ssid",    s_sta_ssid, sizeof(s_sta_ssid));
    s_prefs.getString("pass",    s_sta_pass, sizeof(s_sta_pass));
    s_prefs.getString("ap_ssid", s_ap_ssid,  sizeof(s_ap_ssid));
    s_prefs.getString("ap_pass", s_ap_pass,  sizeof(s_ap_pass));
    s_prefs.end();
    if (strlen(s_ap_ssid) == 0) strncpy(s_ap_ssid, "BRL-Laptimer", sizeof(s_ap_ssid)-1);

    WiFi.persistent(false);

    // Pre-allocate BOTH WiFi interfaces now, while heap is still clean.
    // WIFI_AP_STA keeps the AP and STA VAP objects alive for the entire session.
    // After this single WiFi.mode() call we never change the mode again.
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(true);          // modem sleep — mandatory when BLE also runs

    // Deactivate both interfaces (no SSID/connection) until user enables them.
    WiFi.softAPdisconnect(false); // clear AP config, keep interface allocated
    WiFi.disconnect(false);       // not connected to anything

    g_state.wifi_mode = BRL_WIFI_OFF;
    Serial.println("[WIFI] Manager init (APSTA pre-allocated)");
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

    // Stop current services
    ArduinoOTA.end();
    data_server_stop();
    WiFi.softAPdisconnect(false);
    WiFi.disconnect(false);
    delay(100);

    g_state.wifi_mode = mode;
    strncpy(g_state.wifi_ssid, "", sizeof(g_state.wifi_ssid));

    // NOTE: no WiFi.mode() calls below — we stay in WIFI_AP_STA permanently.
    // Just activate/deactivate the required interface.

    switch (mode) {

        case BRL_WIFI_OFF:
            Serial.println("[WIFI] OFF");
            break;

        case BRL_WIFI_AP: {
            WiFi.setTxPower(WIFI_POWER_19_5dBm);
            delay(50);
            bool ok = WiFi.softAP(s_ap_ssid,
                                  strlen(s_ap_pass) ? s_ap_pass : nullptr,
                                  6,     // channel 6
                                  false, // not hidden
                                  4);    // max 4 clients
            delay(100);
            Serial.printf("[WIFI] AP %s  SSID:%s  IP:%s\n",
                          ok ? "OK" : "FAILED", s_ap_ssid,
                          WiFi.softAPIP().toString().c_str());
            strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
            data_server_start();
            break;
        }

        case BRL_WIFI_STA:
            if (strlen(s_sta_ssid) == 0) {
                Serial.println("[WIFI] No STA credentials");
                g_state.wifi_mode = BRL_WIFI_OFF;
                return;
            }
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
        if (WiFi.status() == WL_CONNECTED && !data_server_running()) {
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
