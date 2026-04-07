/**
 * wifi_mgr.cpp — WiFi manager
 *
 * Architecture: the WiFi AP is started ONCE at boot in wifi_mgr_init(),
 * before NimBLE initialises.  ieee80211_hostap_attach() allocates ESP timers
 * from DRAM; if called after NimBLE/LVGL have run, esp_timer_create() returns
 * ESP_ERR_NO_MEM and the firmware aborts.  By starting the AP early the
 * allocation always succeeds.
 *
 * The UI toggle (BRL_WIFI_AP / BRL_WIFI_OFF) only controls the data server —
 * the AP radio keeps broadcasting for its entire lifetime.
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

    // APSTA: keep both interfaces pre-allocated.
    WiFi.mode(WIFI_AP_STA);
    // Modem sleep is mandatory when BLE also runs (WiFi.mode() resets it to NONE).
    WiFi.setSleep(true);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    // Pre-initialize the AP (visible) so ieee80211_hostap_attach() allocates
    // its ESP timers now, while DRAM is not yet fragmented by NimBLE/LVGL.
    // The data server is NOT started here — user enables it via Settings toggle.
    bool ok = WiFi.softAP(s_ap_ssid,
                           strlen(s_ap_pass) ? s_ap_pass : nullptr,
                           6, false, 4);
    Serial.printf("[WIFI] AP %s  SSID:%s  IP:%s\n",
                  ok ? "started" : "FAILED", s_ap_ssid,
                  WiFi.softAPIP().toString().c_str());

    WiFi.disconnect(false);

    // AP is always on — start the data server immediately.
    data_server_start();
    g_state.wifi_mode = BRL_WIFI_AP;
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
    Serial.println("[WIFI] Manager init done");
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
    WiFi.disconnect(false);
    delay(100);

    g_state.wifi_mode = mode;
    strncpy(g_state.wifi_ssid, "", sizeof(g_state.wifi_ssid));

    switch (mode) {

        case BRL_WIFI_OFF:
            // AP always stays visible — just ensure data server is running.
            strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
            g_state.wifi_mode = BRL_WIFI_AP;
            data_server_start();
            Serial.println("[WIFI] STA disconnected — AP stays active");
            return;   // mode already set above, skip the assignment below

        case BRL_WIFI_AP:
            // Already always on — nothing to do.
            strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
            data_server_start();
            break;

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
    // Update AP config — keep hidden state matching current mode.
    bool hidden = (g_state.wifi_mode != BRL_WIFI_AP);
    WiFi.softAP(s_ap_ssid, strlen(s_ap_pass) ? s_ap_pass : nullptr, 6, hidden, 4);
    Serial.printf("[WIFI] AP config updated: SSID=%s pass=%s\n",
                  s_ap_ssid, strlen(s_ap_pass) ? "***" : "(open)");
}
const char *wifi_ap_pass() { return s_ap_pass; }
