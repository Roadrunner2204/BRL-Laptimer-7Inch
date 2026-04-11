#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * obd_bt -- BLE client for BRL OBD Adapter
 *
 * Uses ESP-IDF NimBLE host API.  On ESP32-P4 the NimBLE host runs on
 * the P4 while the BLE controller runs on the ESP32-C6 co-processor
 * (connected via SDIO / esp_hosted).  The BSP configures this link.
 *
 * Target device: "BRL OBD Adapter"
 * Service : 0000FFE0-0000-1000-8000-00805F9B34FB
 * CMD ch. : 0000FFE1  Write  (Laptimer -> Adapter)
 * RESP ch.: 0000FFE2  Notify (Adapter -> Laptimer)
 *
 * State machine:
 *   IDLE -> SCANNING -> FOUND -> CONNECTING -> CONNECTED -> REQUESTING -> CONNECTED
 *                                                            (loop)
 *   Any error -> ERROR -> IDLE (auto-retry after 5 s)
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OBD_IDLE = 0,
    OBD_SCANNING,
    OBD_FOUND,
    OBD_CONNECTING,
    OBD_CONNECTED,
    OBD_REQUESTING,
    OBD_ERROR
} OBdBtState;

void       obd_bt_init(void);
void       obd_bt_poll(void);    // call in loop -- non-blocking
OBdBtState obd_bt_state(void);
void       obd_bt_disconnect(void);

#ifdef __cplusplus
}
#endif
