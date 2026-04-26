#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi STA bring-up — joins the laptimer's AP so the phone/Studio can
 * pull videos from us via the laptimer's HTTP 302 redirect.
 *
 * Credentials: hard-coded for v1 (must match the laptimer AP). Once
 * pairing is implemented, store them in NVS and provision over UART. */

#define BRL_CAM_AP_SSID  "BRL-Laptimer"
#define BRL_CAM_AP_PASS  ""              /* TODO: provisioning */

void wifi_sta_init(void);
bool wifi_sta_is_up(void);
const char *wifi_sta_get_ip(void);   /* dotted IPv4, "0.0.0.0" if down */

#ifdef __cplusplus
}
#endif
