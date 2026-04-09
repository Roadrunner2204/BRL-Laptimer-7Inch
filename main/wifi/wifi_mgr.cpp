/**
 * wifi_mgr.cpp — WiFi manager STUB for ESP32-P4
 *
 * ESP32-P4 has NO native WiFi hardware. WiFi runs on the ESP32-C6
 * co-processor via esp_hosted (SDIO). The standard esp_wifi API is
 * NOT available.
 *
 * TODO: Implement WiFi using esp_hosted RPC API when ready.
 * For now, all WiFi functions are stubs — the app runs without WiFi.
 */

#include "wifi_mgr.h"
#include "../data/lap_data.h"
#include "compat.h"

static const char *TAG = "wifi_mgr";

static char s_ap_ssid[32] = "BRL-Laptimer";
static char s_ap_pass[64] = {};

void wifi_mgr_init() {
    ESP_LOGW(TAG, "WiFi not yet implemented for ESP32-P4 (requires esp_hosted)");
    g_state.wifi_mode = BRL_WIFI_AP;  // pretend AP is on for UI
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid) - 1);
}

void wifi_mgr_poll() {
    // no-op
}

void wifi_set_mode(WifiMode mode) {
    ESP_LOGW(TAG, "wifi_set_mode(%d) — stubbed", (int)mode);
    g_state.wifi_mode = mode;
}

void wifi_set_sta(const char *ssid, const char *password) {
    ESP_LOGW(TAG, "wifi_set_sta — stubbed");
    (void)ssid; (void)password;
}

const char *wifi_ap_ssid() { return s_ap_ssid; }
const char *wifi_ap_ip()   { return "0.0.0.0"; }

void wifi_ap_set_config(const char *ssid, const char *pass) {
    if (ssid && strlen(ssid) > 0) strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
    if (pass) strncpy(s_ap_pass, pass, sizeof(s_ap_pass) - 1);
    ESP_LOGW(TAG, "wifi_ap_set_config — stubbed");
}

const char *wifi_ap_pass() { return s_ap_pass; }
