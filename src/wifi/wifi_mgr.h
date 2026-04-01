#pragma once
#include <stdint.h>
#include "../data/lap_data.h"  // WifiMode enum

/**
 * wifi_mgr — WiFi manager (AP / STA / OTA)
 *
 * AP mode:  creates "BRL-Laptimer" open network (192.168.4.1)
 *           Android app connects here to download session data
 *
 * STA mode: connects to external WiFi, enables ArduinoOTA
 *
 * OTA mode: firmware update in progress (entered from STA automatically)
 *
 * Usage:
 *   wifi_mgr_init()     — called once in setup()
 *   wifi_mgr_poll()     — called in loop(), handles OTA + reconnect
 *   wifi_set_mode(m)    — switch mode
 *   wifi_set_sta(ssid, pass) — store STA credentials (saved to NVS)
 */

void wifi_mgr_init();
void wifi_mgr_poll();
void wifi_set_mode(WifiMode mode);
void wifi_set_sta(const char *ssid, const char *password);

const char *wifi_ap_ssid();   // returns "BRL-Laptimer"
const char *wifi_ap_ip();     // returns "192.168.4.1"
