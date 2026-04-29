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
#include "../obd/obd_bt.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

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
static bool s_scan_was_ap       = false; // AP was running before scan; restore after
static bool s_scan_was_sta      = false; // STA was running before scan; restore after

// EventGroup for waiting on async WiFi events. esp_wifi_start() and
// esp_wifi_scan_start() are both async over the SDIO/RPC bridge to
// the C6 — calling the next API before the previous one's event has
// been seen produces ESP_ERR_WIFI_STATE / 0-AP results. We sync on
// the events explicitly.
static EventGroupHandle_t s_wifi_events = nullptr;
#define WIFI_BIT_STA_STARTED   BIT0
#define WIFI_BIT_STA_STOPPED   BIT1
#define WIFI_BIT_SCAN_DONE     BIT2

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
            case WIFI_EVENT_AP_START: {
                ESP_LOGI(TAG, "AP started: SSID=%s", s_ap_ssid);
                snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");

                // Reconfigure DHCP here — now that AP is up and DHCPS running.
                // Must stop → set DNS → enable DHCP option 6 → start.
                // Without this, clients default to 8.8.8.8 which fails over
                // mobile data, so Android treats this network as "no internet".
                if (s_netif_ap) {
                    esp_err_t e;
                    e = esp_netif_dhcps_stop(s_netif_ap);
                    ESP_LOGI(TAG, "dhcps_stop: %s", esp_err_to_name(e));

                    esp_netif_dns_info_t dns_info;
                    memset(&dns_info, 0, sizeof(dns_info));
                    IP_ADDR4(&dns_info.ip, 192, 168, 4, 1);
                    e = esp_netif_set_dns_info(s_netif_ap, ESP_NETIF_DNS_MAIN, &dns_info);
                    ESP_LOGI(TAG, "set_dns_info: %s", esp_err_to_name(e));

                    uint8_t offer = 1;  // DHCP option 6 = DNS
                    e = esp_netif_dhcps_option(s_netif_ap,
                                               ESP_NETIF_OP_SET,
                                               ESP_NETIF_DOMAIN_NAME_SERVER,
                                               &offer, sizeof(offer));
                    ESP_LOGI(TAG, "dhcps_option(DNS): %s", esp_err_to_name(e));

                    e = esp_netif_dhcps_start(s_netif_ap);
                    ESP_LOGI(TAG, "dhcps_start: %s", esp_err_to_name(e));
                }

                data_server_start();
                break;
            }

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                data_server_stop();
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                auto *e = (wifi_event_ap_staconnected_t*)data;
                ESP_LOGI(TAG, "✓ Client CONNECTED to AP (AID=%d)", e->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                auto *e = (wifi_event_ap_stadisconnected_t*)data;
                ESP_LOGI(TAG, "Client disconnected (AID=%d)", e->aid);
                break;
            }

            case WIFI_EVENT_STA_START:
                if (s_wifi_events) {
                    xEventGroupClearBits(s_wifi_events, WIFI_BIT_STA_STOPPED);
                    xEventGroupSetBits(s_wifi_events, WIFI_BIT_STA_STARTED);
                }
                if (!s_scanning && g_state.wifi_mode == BRL_WIFI_STA) {
                    ESP_LOGI(TAG, "STA started, connecting...");
                    esp_wifi_connect();
                } else {
                    ESP_LOGI(TAG, "STA started (scan mode, not connecting)");
                }
                break;

            case WIFI_EVENT_STA_STOP:
                if (s_wifi_events) {
                    xEventGroupClearBits(s_wifi_events, WIFI_BIT_STA_STARTED);
                    xEventGroupSetBits(s_wifi_events, WIFI_BIT_STA_STOPPED);
                }
                // Slave hat den STA-Stack runtergefahren — Buchhaltung
                // synchron halten. Sonst skipt wifi_set_mode() den
                // re-init-Pfad weil "läuft ja schon".
                if (g_state.wifi_mode == BRL_WIFI_STA) {
                    s_wifi_started = false;
                }
                break;

            case WIFI_EVENT_SCAN_DONE:
                if (s_wifi_events) {
                    xEventGroupSetBits(s_wifi_events, WIFI_BIT_SCAN_DONE);
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                s_sta_connected = false;
                snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
                data_server_stop();
                if (!s_scanning && g_state.wifi_mode == BRL_WIFI_STA) {
                    ESP_LOGW(TAG, "STA disconnected, reconnecting...");
                    esp_err_t ce = esp_wifi_connect();
                    if (ce == ESP_ERR_WIFI_NOT_STARTED) {
                        // esp_hosted-Verhalten: der C6-Slave fährt den
                        // STA-Stack bei DISCONNECTED komplett runter
                        // (anders als nativer ESP32). Reconnect failt mit
                        // NOT_STARTED — wir müssen den Stack neu hochfahren.
                        // Der STA_START-Handler unten ruft dann automatisch
                        // esp_wifi_connect() auf.
                        ESP_LOGW(TAG, "STA stack was stopped — restarting");
                        s_wifi_started = false;
                        esp_wifi_start();
                    }
                } else if (s_scanning) {
                    ESP_LOGI(TAG, "STA disconnected during scan (ignored)");
                }
                break;

            default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            auto *e = (ip_event_got_ip_t*)data;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
            ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
            s_sta_connected = true;
            data_server_start();
        } else if (id == IP_EVENT_AP_STAIPASSIGNED) {
            auto *e = (ip_event_ap_staipassigned_t*)data;
            ESP_LOGI(TAG, "✓ Client got IP: " IPSTR, IP2STR(&e->ip));
        }
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
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
        g_state.wifi_mode = BRL_WIFI_OFF;
        return;
    }
    s_wifi_started = true;

    // DHCP/DNS reconfig now happens in the WIFI_EVENT_AP_START handler,
    // where dhcps is guaranteed to be running. data_server_start() also
    // runs there so the HTTP + UDP DNS relay come up together.

    g_state.wifi_mode = BRL_WIFI_AP;
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid) - 1);
    ESP_LOGI(TAG, "wifi_start_ap: requested — waiting for AP_START event");
}

// ---------------------------------------------------------------------------
// Internal: start STA mode
// ---------------------------------------------------------------------------
static void wifi_start_sta(void)
{
    // Mode switches into STA also stress the C6 — pause BLE-discovery
    // here too. The pause is undone at the end of this function (or by
    // wifi_set_mode-callers) since STA stays active afterwards.
    obd_bt_pause(true);
    if (strlen(s_sta_ssid) == 0) {
        ESP_LOGW(TAG, "No STA credentials configured");
        obd_bt_pause(false);
        return;
    }

    wifi_stop();

    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, s_sta_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, s_sta_pass, sizeof(cfg.sta.password) - 1);
    // Auth threshold: many home routers refuse a station that announces
    // WIFI_AUTH_OPEN as its minimum (the default of a zeroed wifi_config_t)
    // when their own mode is WPA2/WPA3. Setting WPA-PSK as the minimum
    // covers the vast majority of consumer routers and still allows OPEN
    // on hotel/guest networks (downward compatible).
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    // PMF (Protected Management Frames) — capable but not required, so
    // we connect to both WPA2-only routers and WPA3/WPA2-mixed APs.
    cfg.sta.pmf_cfg.capable     = true;
    cfg.sta.pmf_cfg.required    = false;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    s_wifi_started = true;

    g_state.wifi_mode = BRL_WIFI_STA;
    strncpy(g_state.wifi_ssid, s_sta_ssid, sizeof(g_state.wifi_ssid) - 1);
    ESP_LOGI(TAG, "STA mode started: connecting to %s", s_sta_ssid);
    // STA is steady-state from here on — BLE may resume scanning. The
    // concurrency that crashed us was the *transient* mode-switch +
    // active scanning, not just having STA up.
    obd_bt_pause(false);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void wifi_mgr_init(void)
{
    nvs_load_wifi_config();

    s_wifi_events = xEventGroupCreate();

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
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr);

    // Country code — the laptimer ships worldwide (rally cars, customer
    // installs in US/EU/JP/AUS). Hard-coding a single country would lock
    // out anyone outside it, so we use ESP-IDF's "World Safe Mode":
    //   cc = "01"      → ESP-IDF identifier for "country unknown"
    //   schan/nchan = 1..13 → permissive scan range covering EU + JP
    //   policy = AUTO  → the *moment* we connect to any AP, the C6
    //                    parses its beacon's Country IE and switches
    //                    our regulatory domain to match (US AP →
    //                    nchan=11, JP AP → nchan=14, etc.). On
    //                    disconnect we fall back to this World-Safe
    //                    default for the next scan.
    //
    // Net effect: the initial Settings-Scan finds APs anywhere in the
    // world, and once the user picks one we automatically operate
    // within that country's legal channel set. No per-region build,
    // no setting the user has to touch.
    wifi_country_t country = {};
    memcpy(country.cc, "01", 2);
    country.schan        = 1;
    country.nchan        = 13;
    country.policy       = WIFI_COUNTRY_POLICY_AUTO;
    esp_wifi_set_country(&country);

    g_state.wifi_mode = BRL_WIFI_OFF;
    strncpy(g_state.wifi_ssid, s_ap_ssid, sizeof(g_state.wifi_ssid) - 1);

    ESP_LOGI(TAG, "WiFi init OK (via esp_hosted → ESP32-C6, "
                  "country=World-Safe ch1-13 + AUTO policy)");
}

void wifi_mgr_poll(void)
{
    // Event-driven — nothing to poll
}

void wifi_set_mode(WifiMode mode)
{
    // Idempotent only when the stack is *actually* in that mode. After a
    // disconnect-induced STA stop, g_state.wifi_mode may still say STA but
    // s_wifi_started is false — we must fall through to re-init, otherwise
    // a user who clicks Save-and-Connect again on the same SSID gets
    // silently nothing because the early-return ate the request.
    if (mode == g_state.wifi_mode && s_wifi_started) return;

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
    // Tell OBD-BT to stand down. WiFi scans + BLE discovery hammering the
    // shared ESP32-C6 over esp_hosted at the same time produced HCI
    // timeouts → BLE host resets → eventually a Core 0 panic. Pausing
    // BLE scanning during the scan is the cheap, reliable fix.
    obd_bt_pause(true);
    s_scanning = true;
    s_scan_started_wifi = !s_wifi_started;

    // Switch to STA-only for the duration of the scan. Earlier we kept
    // the AP up via APSTA so clients wouldn't drop, but on esp_hosted
    // the C6 cannot reliably interleave AP-beacons (channel 6) with
    // probe-requests across all 13 channels — beacons starve the scan
    // and only a handful of APs (or none) come back. Dropping AP for
    // the ~6 s scan window costs the user a momentary disconnect of the
    // phone, which is the lesser evil compared to "Settings → Scan
    // never finds anything".
    s_scan_was_ap  = (g_state.wifi_mode == BRL_WIFI_AP);
    s_scan_was_sta = (g_state.wifi_mode == BRL_WIFI_STA);
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
        s_scan_started_wifi = true;  // we'll need to bring it back up after
    }

    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events,
                             WIFI_BIT_STA_STARTED |
                             WIFI_BIT_STA_STOPPED |
                             WIFI_BIT_SCAN_DONE);
    }
    esp_wifi_set_mode(WIFI_MODE_STA);

    // esp_hosted-Slave verlangt eine gesetzte STA-Config bevor er Scans
    // akzeptiert. Auf nativem ESP32 ist set_config für reinen Scan
    // optional — der C6-Slave wirft sonst ESP_ERR_WIFI_STATE (0x3006)
    // zurück. Wir setzen einen leeren Config nur damit der Slave-State
    // valide ist; verbinden tun wir hier nichts.
    {
        wifi_config_t empty_sta = {};
        esp_wifi_set_config(WIFI_IF_STA, &empty_sta);
    }

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(STA) for scan failed: %s",
                 esp_err_to_name(ret));
        s_scanning = false;
        s_scan_started_wifi = false;
        if (s_scan_was_ap)       wifi_start_ap();
        else if (s_scan_was_sta) wifi_start_sta();
        s_scan_was_ap = s_scan_was_sta = false;
        obd_bt_pause(false);
        return 0;
    }
    s_wifi_started = true;

    // Auf STA_START warten — esp_wifi_start ist async über die SDIO/RPC.
    if (s_wifi_events) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                               WIFI_BIT_STA_STARTED,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(3000));
        if (!(bits & WIFI_BIT_STA_STARTED)) {
            ESP_LOGW(TAG, "STA_START event timed out — trying scan anyway");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = true;
    scan_cfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 120;
    scan_cfg.scan_time.active.max = 450;

    uint16_t count = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        // Defensive: clear any stale half-finished scan on the slave.
        // esp_wifi_scan_stop returns ESP_ERR_WIFI_NOT_STARTED when
        // nothing was running — that's fine and we ignore it.
        esp_wifi_scan_stop();

        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_BIT_SCAN_DONE);
        }

        // Non-blocking scan + wait on WIFI_EVENT_SCAN_DONE. Blocking
        // mode on esp_hosted has shown ESP_ERR_WIFI_STATE responses
        // even when the slave was actually ready — the event-based
        // path is the more reliable one.
        ret = esp_wifi_scan_start(&scan_cfg, false);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "scan_start (attempt %d) failed: %s",
                     attempt + 1, esp_err_to_name(ret));
            // Wait + retry. Slave can be in a transient busy state.
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 13 channels × 450 ms = ~6 s worst case + slack
        EventBits_t bits = 0;
        if (s_wifi_events) {
            bits = xEventGroupWaitBits(s_wifi_events, WIFI_BIT_SCAN_DONE,
                                       pdTRUE, pdFALSE,
                                       pdMS_TO_TICKS(8000));
        }
        if (s_wifi_events && !(bits & WIFI_BIT_SCAN_DONE)) {
            ESP_LOGW(TAG, "SCAN_DONE timeout (attempt %d)", attempt + 1);
            esp_wifi_scan_stop();
            continue;
        }

        esp_wifi_scan_get_ap_num(&count);
        ESP_LOGI(TAG, "Scan attempt %d: %d APs", attempt + 1, (int)count);
        if (count > 0) break;

        // Empty result: drain any record the slave staged so the next
        // scan_start has a clean slot, then retry.
        uint16_t dummy = 0;
        esp_wifi_scan_get_ap_records(&dummy, nullptr);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (count == 0) {
        // No results — cleanup now since get_results won't be called
        esp_wifi_stop();
        s_wifi_started = false;
        s_scanning = false;
        s_scan_started_wifi = false;
        if (s_scan_was_ap)       wifi_start_ap();
        else if (s_scan_was_sta) wifi_start_sta();
        s_scan_was_ap = s_scan_was_sta = false;
        obd_bt_pause(false);
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

        // Restore WiFi state after scan results are retrieved. STA was
        // brought up only for the scan; tear it back down and put AP
        // back if it was running before. Caller normally follows up with
        // wifi_set_mode(STA) once the user picks an SSID — that path
        // does its own start, so we don't need to leave STA up here.
        esp_wifi_stop();
        s_wifi_started = false;
        if (s_scan_was_ap)       wifi_start_ap();
        else if (s_scan_was_sta) wifi_start_sta();
        s_scan_was_ap = s_scan_was_sta = false;
        s_scanning = false;
        s_scan_started_wifi = false;
        obd_bt_pause(false);

        return n;
    }

cleanup:
    esp_wifi_stop();
    s_wifi_started = false;
    if (s_scan_was_ap)       wifi_start_ap();
    else if (s_scan_was_sta) wifi_start_sta();
    s_scan_was_ap = s_scan_was_sta = false;
    s_scanning = false;
    s_scan_started_wifi = false;
    obd_bt_pause(false);
    return 0;
}
