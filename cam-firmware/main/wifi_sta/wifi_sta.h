#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wifi_sta — joins the laptimer's AP via the onboard ESP32-C6
 * (esp_hosted, SDIO).
 *
 * The cam exposes its HTTP server on this connection; the laptimer's
 * data_server hands clients a 302 redirect to <our IP>:80. Reconnect
 * is handled internally — the laptimer might be off when the cam
 * boots, or the AP cycles when the user toggles WiFi mode.
 *
 * SSID / passphrase are compile-time constants for v1; provisioning
 * over the cam_link UART can be added later.
 */

#define BRL_CAM_AP_SSID  "BRL-Laptimer"
#define BRL_CAM_AP_PASS  ""              /* TODO: provisioning */

void wifi_sta_init(void);
bool wifi_sta_is_up(void);
const char *wifi_sta_get_ip(void);   /* dotted IPv4, "0.0.0.0" if down */

#ifdef __cplusplus
}
#endif
