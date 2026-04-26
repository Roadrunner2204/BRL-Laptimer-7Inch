/**
 * wifi_sta.c — STUB. Joins the laptimer's AP via the onboard ESP32-C6
 * (esp_hosted, SDIO). Real impl pending — see TODO below.
 *
 * The wifi_mgr in the main laptimer firmware is a good template
 * (../../main/wifi/wifi_mgr.cpp). The cam side only needs STA mode.
 */

#include "wifi_sta.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_sta";

static bool s_up = false;
static char s_ip[16] = "0.0.0.0";

void wifi_sta_init(void) {
    ESP_LOGI(TAG, "init (stub) — TODO: esp_hosted bring-up + STA join");
    /* TODO:
     *   1. nvs_flash_init (already done in app_main)
     *   2. esp_netif_init / esp_event_loop_create_default
     *   3. esp_wifi_init with esp_hosted slave config
     *   4. esp_wifi_set_mode(WIFI_MODE_STA)
     *   5. esp_wifi_set_config(WIFI_IF_STA, { ssid=BRL_CAM_AP_SSID, ... })
     *   6. esp_wifi_start + esp_wifi_connect
     *   7. on IP_EVENT_STA_GOT_IP: snprintf s_ip from ip_info.ip; s_up = true
     */
}

bool wifi_sta_is_up(void)        { return s_up; }
const char *wifi_sta_get_ip(void){ return s_ip; }
