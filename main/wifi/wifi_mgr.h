#pragma once
#include <stdint.h>
#include "../data/lap_data.h"  // WifiMode enum

/**
 * wifi_mgr -- WiFi manager (AP / STA / OTA)
 *
 * AP mode:  creates "BRL-Laptimer" open network (192.168.4.1)
 *           Android app connects here to download session data
 *
 * STA mode: connects to external WiFi
 *
 * OTA mode: firmware update in progress (stub -- esp_https_ota later)
 *
 * The ESP32-P4 has no built-in WiFi; an ESP32-C6 co-processor is
 * connected via SDIO (esp_hosted).  The standard esp_wifi API works
 * transparently on top of esp_hosted.
 *
 * Usage:
 *   wifi_mgr_init()     -- called once during startup, BEFORE obd_bt_init()
 *   wifi_mgr_poll()     -- called in main loop
 *   wifi_set_mode(m)    -- switch mode
 *   wifi_set_sta(ssid, pass) -- store STA credentials (saved to NVS)
 */

#ifdef __cplusplus
extern "C" {
#endif

void wifi_mgr_init(void);
void wifi_mgr_poll(void);
void wifi_set_mode(WifiMode mode);
void wifi_set_sta(const char *ssid, const char *password);

const char *wifi_ap_ssid(void);
const char *wifi_ap_ip(void);
void wifi_ap_set_config(const char *ssid, const char *pass);
const char *wifi_ap_pass(void);

// WiFi scan — returns number of APs found (max max_results)
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;   // 0=open, else secured
} WifiScanResult;

int  wifi_scan_start(void);                           // blocking scan, returns count
int  wifi_scan_get_results(WifiScanResult *out, int max_results);

#ifdef __cplusplus
}
#endif
