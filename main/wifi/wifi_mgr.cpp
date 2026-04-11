/**
 * wifi_mgr.cpp -- WiFi manager for ESP32-P4 via esp_hosted (ESP32-C6)
 *
 * Uses standard esp_wifi API which is transparently proxied to the
 * ESP32-C6 co-processor via esp_wifi_remote / esp_hosted over SDIO.
 *
 * Modes:
 *   BRL_WIFI_AP  -- "BRL-Laptimer" hotspot (192.168.4.1), data server active
 *   BRL_WIFI_STA -- connect to external WiFi (for OTA updates)
 *   BRL_WIFI_OFF -- radio off, data server stopped
 */

#include "wifi_mgr.h"
#include "data_server.h"
#include "../data/lap_data.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "wifi_mgr";

#define NVS_WIFI_NS  "wifi_cfg"

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static char s_ap_ssid[32] = "BRL-Laptimer";
static char s_ap_pass[64] = {};
static char s_sta_ssid[32] = {};
static char s_sta_pass[64] = {};
static char s_ip_str[16]   = "0.0.0.0";

static esp_netif_t *s_netif_ap  = nullptr;
static esp_netif_t *s_netif_sta = nullptr;
static bool s_wifi_started = false;
static bool s_sta_connected = false;
static bool s_scanning = false;      // true during scan (suppresses connect/reconnect)
static bool s_scan_started_wifi = false; // true if scan started WiFi (needs cleanup)

// ---------------------------------------------------------------------------
// NVS helpers for WiFi credentials
// ---------------------------------------------------------------------------
static void nvs_load_wifi_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len;
    len = sizeof(s_ap_ssid);  nvs_get_str(h, "ap_ssid", s_ap_ssid, &len);
    len = sizeof(s_ap_pass);  nvs_get_str(h, "ap_pass", s_ap_pass, &len);
    len = sizeof(s_sta_ssid); nvs_get_str(h, "sta_ssid", s_sta_ssid, &len);
    len = sizeof(s_sta_pass); nvs_get_str(h, "sta_pass", s_sta_pass, &len);

    nvs_close(h);
}

static void nvs_save_wifi_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_WIFI_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_str(h, "ap_ssid", s_ap_ssid);
    nvs_set_str(h, "ap_pass", s_ap_pass);
    nvs_set_str(h, "sta_ssid", s_sta_ssid);
    nvs_set_str(h, "sta_pass", s_sta_pass);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started: SSID=%s", s_ap_ssid);
                snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
                data_server_start();
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                data_server_stop();
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                auto *e = (wifi_event_ap_staconnected_t*)data;
                ESP_LOGI(TAG, "Client connected (AID=%d)", e->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                auto *e = (wifi_event_ap_stadisconnected_t*)data;
                ESP_LOGI(TAG, "Client disconnected (AID=%d)", e->aid);
                break;
            }

            case WIFI_EVENT_STA_START:
                if (!s_scanning && g_state.wifi_mode == BRL_WIFI_STA) {
                    ESP_LOGI(TAG, "STA started, connecting...");
                    esp_wifi_connect();
                } else {
                    ESP_LOGI(TAG, "STA started (scan mode, not connecting)");
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                s_sta_connected = false;
                snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
                data_server_stop();
                if (!s_scanning && g_state.wifi_mode == BRL_WIFI_STA) {
                    ESP_LOGW(TAG, "STA disconnected, reconnecting...");
                    esp_wifi_connect();
                } else if (s_scanning) {
                    ESP_LOGI(TAG, "STA disconnected during scan (ignored)");
                }
                break;

            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = (ip_event_got_ip_t*)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
        s_sta_connected = true;
        data_server_start();
    }
}

// ---------------------------------------------------------------------------
// Internal: stop WiFi cleanly
// ---------------------------------------------------------------------------
static void wifi_stop(void)
{
    if (!s_wifi_started) return;

    data_server_stop();
    esp_wifi_stop();
    s_wifi_started  = false;
    s_sta_connected = false;
    snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
}

// ---------------------------------------------------------------------------
// Internal: start AP mode
// ---------------------------------------------------------------------------
static void wifi_start_ap(void)
{
    wifi_stop();

    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t cfg = {};
    strncpy((char*)cfg.ap.ssid, s_ap_ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    cfg.ap.max_connection = 4;
    cfg.ap.channel = 6;

    if (strlen(s_ap_pass) >= 8) {
        strncpy((char*)cfg.ap.password, s_ap_pass, sizeof(cfg.ap.password) - 1);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_wifi_start();
    s_wifi_started = true;

    g_state.wifi_mode = BRL_WIFI_AP;
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid) - 1);
    ESP_LOGI(TAG, "AP mode started: %s", s_ap_ssid);
}

// ---------------------------------------------------------------------------
// Internal: start STA mode
// ---------------------------------------------------------------------------
static void wifi_start_sta(void)
{
    if (strlen(s_sta_ssid) == 0) {
        ESP_LOGW(TAG, "No STA credentials configured");
        return;
    }

    wifi_stop();

    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, s_sta_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, s_sta_pass, sizeof(cfg.sta.password) - 1);

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    s_wifi_started = true;

    g_state.wifi_mode = BRL_WIFI_STA;
    strncpy(g_state.wifi_ssid, s_sta_ssid, sizeof(g_state.wifi_ssid) - 1);
    ESP_LOGI(TAG, "STA mode started: connecting to %s", s_sta_ssid);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void wifi_mgr_init(void)
{
    nvs_load_wifi_config();

    // Network interface + event loop
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
    }

    // Create default network interfaces
    s_netif_ap  = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    // Initialize WiFi (proxied to C6 via esp_hosted)
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        g_state.wifi_mode = BRL_WIFI_OFF;
        return;
    }

    // Register event handlers
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr);

    g_state.wifi_mode = BRL_WIFI_OFF;
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid) - 1);

    ESP_LOGI(TAG, "WiFi init OK (via esp_hosted → ESP32-C6)");
}

void wifi_mgr_poll(void)
{
    // Event-driven — nothing to poll
}

void wifi_set_mode(WifiMode mode)
{
    if (mode == g_state.wifi_mode) return;

    switch (mode) {
        case BRL_WIFI_AP:
            wifi_start_ap();
            break;
        case BRL_WIFI_STA:
            wifi_start_sta();
            break;
        case BRL_WIFI_OFF:
        default:
            wifi_stop();
            g_state.wifi_mode = BRL_WIFI_OFF;
            ESP_LOGI(TAG, "WiFi OFF");
            break;
    }
}

void wifi_set_sta(const char *ssid, const char *password)
{
    if (ssid) {
        strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
        s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
    }
    if (password) {
        strncpy(s_sta_pass, password, sizeof(s_sta_pass) - 1);
        s_sta_pass[sizeof(s_sta_pass) - 1] = '\0';
    }
    nvs_save_wifi_config();
    ESP_LOGI(TAG, "STA credentials saved: SSID=%s", s_sta_ssid);
}

const char *wifi_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_ap_ip(void)   { return s_ip_str; }
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
    nvs_save_wifi_config();
    ESP_LOGI(TAG, "AP config saved: SSID=%s", s_ap_ssid);

    // If AP is currently running, restart with new config
    if (g_state.wifi_mode == BRL_WIFI_AP) {
        wifi_start_ap();
    }
}

// ---------------------------------------------------------------------------
// WiFi scan — temporarily switches to STA, scans, then restores previous mode
// ---------------------------------------------------------------------------
int wifi_scan_start(void)
{
    s_scanning = true;
    s_scan_started_wifi = !s_wifi_started;

    // Must be in STA (or APSTA) mode to scan
    if (!s_wifi_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        s_wifi_started = true;
    } else if (g_state.wifi_mode == BRL_WIFI_AP) {
        // Switch to APSTA so AP stays active while scanning
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true); // blocking
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        // Cleanup immediately on failure
        if (s_scan_started_wifi) {
            esp_wifi_stop();
            s_wifi_started = false;
        } else if (g_state.wifi_mode == BRL_WIFI_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        s_scanning = false;
        s_scan_started_wifi = false;
        return 0;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    ESP_LOGI(TAG, "Scan found %d APs", (int)count);

    if (count == 0) {
        // No results — cleanup now since get_results won't be called
        if (s_scan_started_wifi) {
            esp_wifi_stop();
            s_wifi_started = false;
        } else if (g_state.wifi_mode == BRL_WIFI_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        s_scanning = false;
        s_scan_started_wifi = false;
    }
    // If count > 0, caller must call wifi_scan_get_results() which does cleanup
    return (int)count;
}

int wifi_scan_get_results(WifiScanResult *out, int max_results)
{
    if (!out || max_results <= 0) {
        // Still need to cleanup scan state even if no results requested
        goto cleanup;
    }

    {
        uint16_t count = (uint16_t)max_results;
        wifi_ap_record_t *records = (wifi_ap_record_t*)calloc(count, sizeof(wifi_ap_record_t));
        if (!records) goto cleanup;

        esp_wifi_scan_get_ap_records(&count, records);

        int n = (count < (uint16_t)max_results) ? count : max_results;
        for (int i = 0; i < n; i++) {
            strncpy(out[i].ssid, (const char*)records[i].ssid, sizeof(out[i].ssid) - 1);
            out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
            out[i].rssi = records[i].rssi;
            out[i].authmode = (records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
        }

        free(records);

        // Restore WiFi state after scan results are retrieved
        if (s_scan_started_wifi) {
            esp_wifi_stop();
            s_wifi_started = false;
        } else if (g_state.wifi_mode == BRL_WIFI_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        s_scanning = false;
        s_scan_started_wifi = false;

        return n;
    }

cleanup:
    if (s_scan_started_wifi) {
        esp_wifi_stop();
        s_wifi_started = false;
    } else if (g_state.wifi_mode == BRL_WIFI_AP) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    s_scanning = false;
    s_scan_started_wifi = false;
    return 0;
}
