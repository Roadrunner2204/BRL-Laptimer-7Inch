/**
 * wifi_mgr.cpp -- WiFi manager (ESP-IDF / ESP32-P4)
 *
 * Replaces the Arduino WiFi.h + ArduinoOTA + Preferences implementation.
 * Uses esp_wifi for AP/STA, NVS for credential storage, esp_netif for
 * network interface management.
 *
 * The ESP32-P4 has no built-in WiFi.  An ESP32-C6 co-processor connected
 * via SDIO provides the radio (esp_hosted).  The standard esp_wifi API
 * works transparently on top of this -- the Waveshare BSP handles the
 * SDIO transport layer.
 *
 * Architecture: the WiFi AP is started ONCE at boot in wifi_mgr_init(),
 * before NimBLE initialises.  Caller must invoke wifi_mgr_init() BEFORE
 * obd_bt_init().
 *
 * The AP is always visible.  The data server starts at boot and runs
 * forever.
 */

#include "wifi_mgr.h"
#include "data_server.h"
#include "../data/lap_data.h"
#include "compat.h"

#include <string.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "wifi_mgr";

// ---------------------------------------------------------------------------
// NVS namespace and keys
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE   "wifi"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static char s_sta_ssid[64] = {};
static char s_sta_pass[64] = {};
static char s_ap_ssid[32]  = {};
static char s_ap_pass[64]  = {};

static esp_netif_t *s_ap_netif  = nullptr;
static esp_netif_t *s_sta_netif = nullptr;
static bool         s_wifi_started = false;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static void nvs_load_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    size_t len;

    len = sizeof(s_sta_ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, s_sta_ssid, &len) != ESP_OK)
        s_sta_ssid[0] = '\0';

    len = sizeof(s_sta_pass);
    if (nvs_get_str(h, NVS_KEY_PASS, s_sta_pass, &len) != ESP_OK)
        s_sta_pass[0] = '\0';

    len = sizeof(s_ap_ssid);
    if (nvs_get_str(h, NVS_KEY_AP_SSID, s_ap_ssid, &len) != ESP_OK)
        s_ap_ssid[0] = '\0';

    len = sizeof(s_ap_pass);
    if (nvs_get_str(h, NVS_KEY_AP_PASS, s_ap_pass, &len) != ESP_OK)
        s_ap_pass[0] = '\0';

    nvs_close(h);
}

static void nvs_save_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// WiFi event handler
// ---------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA connected to AP");
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "STA disconnected");
                if (g_state.wifi_mode == BRL_WIFI_STA) {
                    // Auto-reconnect
                    esp_wifi_connect();
                }
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *ev =
                    (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "AP: station " MACSTR " joined, AID=%d",
                         MAC2STR(ev->mac), ev->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *ev =
                    (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "AP: station " MACSTR " left, AID=%d",
                         MAC2STR(ev->mac), ev->aid);
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
            if (!data_server_running()) {
                data_server_start();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Start soft-AP with current config
// ---------------------------------------------------------------------------
static void start_soft_ap(void)
{
    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel  = 6;
    ap_cfg.ap.max_connection = 4;

    if (strlen(s_ap_pass) >= 8) {
        strncpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.password[0] = '\0';
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_LOGI(TAG, "softAP configured: SSID=%s channel=%d auth=%s",
             s_ap_ssid, ap_cfg.ap.channel,
             ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void wifi_mgr_init(void)
{
    // --- Initialize NVS (required for esp_wifi) ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // --- Load saved credentials ---
    nvs_load_credentials();
    if (strlen(s_ap_ssid) == 0) {
        strncpy(s_ap_ssid, "BRL-Laptimer", sizeof(s_ap_ssid) - 1);
    }

    // --- Initialize TCP/IP stack and event loop ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- Create default netif for AP and STA ---
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // --- Initialize WiFi with default config ---
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // --- Register event handlers ---
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    // --- Set APSTA mode (AP always visible, STA can connect later) ---
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // --- Configure and start soft-AP ---
    start_soft_ap();

    // --- Start WiFi ---
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    // --- Disconnect STA side (we only want AP initially) ---
    esp_wifi_disconnect();

    // --- Start the data server immediately (AP is always on) ---
    data_server_start();

    g_state.wifi_mode = BRL_WIFI_AP;
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));

    ESP_LOGI(TAG, "init done -- AP SSID: %s, server at http://192.168.4.1/", s_ap_ssid);
}

// ---------------------------------------------------------------------------
void wifi_set_sta(const char *ssid, const char *password)
{
    strncpy(s_sta_ssid, ssid,     sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    strncpy(s_sta_pass, password, sizeof(s_sta_pass) - 1);
    s_sta_pass[sizeof(s_sta_pass) - 1] = '\0';

    nvs_save_str(NVS_KEY_SSID, s_sta_ssid);
    nvs_save_str(NVS_KEY_PASS, s_sta_pass);
}

// ---------------------------------------------------------------------------
void wifi_set_mode(WifiMode mode)
{
    if (g_state.wifi_mode == mode) return;

    // Stop data server (will be restarted as needed)
    data_server_stop();

    // Disconnect STA side
    esp_wifi_disconnect();
    delay(100);

    g_state.wifi_mode = mode;
    g_state.wifi_ssid[0] = '\0';

    switch (mode) {

        case BRL_WIFI_OFF:
            // AP always stays visible -- fall through to AP mode
            strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
            g_state.wifi_mode = BRL_WIFI_AP;
            data_server_start();
            ESP_LOGI(TAG, "STA disconnected -- AP stays active");
            return;

        case BRL_WIFI_AP:
            strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
            data_server_start();
            ESP_LOGI(TAG, "Mode: AP");
            break;

        case BRL_WIFI_STA: {
            if (strlen(s_sta_ssid) == 0) {
                ESP_LOGW(TAG, "No STA credentials stored");
                g_state.wifi_mode = BRL_WIFI_AP;
                strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
                data_server_start();
                return;
            }

            // Configure STA
            wifi_config_t sta_cfg = {};
            strncpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
            strncpy((char *)sta_cfg.sta.password, s_sta_pass, sizeof(sta_cfg.sta.password) - 1);
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
            esp_wifi_connect();

            strncpy(g_state.wifi_ssid, s_sta_ssid, sizeof(g_state.wifi_ssid));
            ESP_LOGI(TAG, "Connecting STA to %s...", s_sta_ssid);

            // Data server will start once STA gets an IP (via event handler)
            // AP data server also stays available
            data_server_start();
            break;
        }

        case BRL_WIFI_OTA:
            // OTA stub -- esp_https_ota could be integrated here later
            ESP_LOGI(TAG, "OTA mode requested -- not yet implemented on ESP32-P4");
            ESP_LOGI(TAG, "Use 'idf.py flash' or esp_https_ota for firmware updates");
            // Fall back to AP mode
            g_state.wifi_mode = BRL_WIFI_AP;
            strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid));
            data_server_start();
            break;
    }
}

// ---------------------------------------------------------------------------
void wifi_mgr_poll(void)
{
    // The esp_wifi event system handles reconnection via the event handler.
    // ArduinoOTA is replaced by esp_https_ota (stubbed).
    // DNS polling for captive portal is handled in data_server_poll().

    // In STA mode, check if data server needs starting (fallback if event missed)
    if (g_state.wifi_mode == BRL_WIFI_STA && !data_server_running()) {
        esp_netif_ip_info_t ip_info;
        if (s_sta_netif &&
            esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "STA has IP, starting data server");
            data_server_start();
        }
    }
}

// ---------------------------------------------------------------------------
const char *wifi_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_ap_ip(void)   { return "192.168.4.1"; }
const char *wifi_ap_pass(void) { return s_ap_pass; }

// ---------------------------------------------------------------------------
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

    nvs_save_str(NVS_KEY_AP_SSID, s_ap_ssid);
    nvs_save_str(NVS_KEY_AP_PASS, s_ap_pass);

    // Re-apply AP configuration live
    if (s_wifi_started) {
        start_soft_ap();
    }

    ESP_LOGI(TAG, "AP config updated: SSID=%s pass=%s",
             s_ap_ssid, strlen(s_ap_pass) ? "***" : "(open)");
}
