/**
 * wifi_sta.c — STA bring-up + reconnect on the onboard ESP32-C6.
 *
 * Uses standard esp_wifi APIs; esp_wifi_remote / esp_hosted route the
 * calls over SDIO to the C6 transparently (component manager pulls in
 * the right shims via idf_component.yml).
 *
 * Reconnect strategy: on STA_DISCONNECTED, schedule esp_wifi_connect()
 * after a short backoff so we don't busy-loop while the laptimer's AP
 * is down. Backoff capped at 10 s — short enough that the user notices
 * the cam pop back online when they toggle WiFi mode on the laptimer.
 */

#include "wifi_sta.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static const char *TAG = "wifi_sta";

static esp_netif_t   *s_netif = NULL;
static bool           s_up = false;
static char           s_ip[16] = "0.0.0.0";
static TimerHandle_t  s_reconnect_timer = NULL;
static uint32_t       s_backoff_ms = 1000;

#define BACKOFF_MIN_MS  1000
#define BACKOFF_MAX_MS  10000

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer) return;
    xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_backoff_ms), 0);
    xTimerStart(s_reconnect_timer, 0);
    s_backoff_ms *= 2;
    if (s_backoff_ms > BACKOFF_MAX_MS) s_backoff_ms = BACKOFF_MAX_MS;
}

static void reconnect_cb(TimerHandle_t t)
{
    (void)t;
    ESP_LOGI(TAG, "reconnect attempt");
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t event_id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started, connecting to '%s'", BRL_CAM_AP_SSID);
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_up) ESP_LOGW(TAG, "disconnected — will retry");
        s_up = false;
        strncpy(s_ip, "0.0.0.0", sizeof(s_ip));
        schedule_reconnect();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "associated — waiting for DHCP");
        break;

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t event_id, void *data)
{
    (void)arg; (void)base;
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_up = true;
        s_backoff_ms = BACKOFF_MIN_MS;
        ESP_LOGI(TAG, "got IP %s", s_ip);
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        s_up = false;
        strncpy(s_ip, "0.0.0.0", sizeof(s_ip));
        ESP_LOGW(TAG, "lost IP");
    }
}

void wifi_sta_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    /* esp_event_loop_create_default may have been created by the caller
     * (main.c). esp_event tolerates the second call returning
     * ESP_ERR_INVALID_STATE — ignore that one specifically. */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL, NULL));

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid,     BRL_CAM_AP_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, BRL_CAM_AP_PASS, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = (BRL_CAM_AP_PASS[0] == '\0')
        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta.sta.pmf_cfg.capable    = true;
    sta.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    s_reconnect_timer = xTimerCreate("wifi_sta_rc", pdMS_TO_TICKS(BACKOFF_MIN_MS),
                                     pdFALSE, NULL, reconnect_cb);

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_sta_init done — target SSID '%s'", BRL_CAM_AP_SSID);
}

bool        wifi_sta_is_up(void)  { return s_up; }
const char *wifi_sta_get_ip(void) { return s_ip; }
