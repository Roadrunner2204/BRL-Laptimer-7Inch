/**
 * wifi_mgr.cpp -- WiFi manager STUB for ESP32-P4
 *
 * The ESP32-P4 has NO native WiFi hardware.  WiFi is provided by the
 * ESP32-C6 co-processor via the esp_hosted component, which uses a
 * different API from the standard esp_wifi driver.
 *
 * Until esp_hosted is properly integrated, all WiFi functions are
 * stubbed out as no-ops so the rest of the firmware can build and run.
 */

#include "wifi_mgr.h"
#include "../data/lap_data.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "wifi_mgr";

static char s_ap_ssid[32] = "BRL-Laptimer";
static char s_ap_pass[64] = {};

void wifi_mgr_init(void)
{
    ESP_LOGW(TAG, "WiFi not yet implemented for ESP32-P4 (requires esp_hosted setup)");
    g_state.wifi_mode = BRL_WIFI_OFF;
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid) - 1);
    ESP_LOGI(TAG, "WiFi stub init done -- mode=OFF");
}

void wifi_mgr_poll(void)
{
    // No-op: WiFi not available on ESP32-P4 without esp_hosted
}

void wifi_set_mode(WifiMode mode)
{
    (void)mode;
    ESP_LOGW(TAG, "wifi_set_mode: WiFi not available on ESP32-P4 (stub)");
    g_state.wifi_mode = BRL_WIFI_OFF;
}

void wifi_set_sta(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    ESP_LOGW(TAG, "wifi_set_sta: WiFi not available on ESP32-P4 (stub)");
}

const char *wifi_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_ap_ip(void)   { return "0.0.0.0"; }
const char *wifi_ap_pass(void) { return s_ap_pass; }

void wifi_ap_set_config(const char *ssid, const char *pass)
{
    if (ssid && strlen(ssid) > 0) {
        strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
        s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
    }
    if (pass) {
        strncpy(s_ap_pass, pass, sizeof(s_ap_pass) - 1);
        s_ap_pass[sizeof(s_ap_pass) - 1] = '\0';
    }
    ESP_LOGI(TAG, "AP config saved (will apply when WiFi is implemented): SSID=%s",
             s_ap_ssid);
}
