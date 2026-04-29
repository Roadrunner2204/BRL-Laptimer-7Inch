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
    OBD_FINGERPRINTING,   // post-subscribe, reading PID 0x00 to ID the ECU
    OBD_CONNECTED,
    OBD_REQUESTING,
    OBD_ERROR
} OBdBtState;

void       obd_bt_init(void);
void       obd_bt_poll(void);    // call in loop -- non-blocking
OBdBtState obd_bt_state(void);
void       obd_bt_disconnect(void);

/**
 * Pause/resume cooperation with the WiFi-Manager.
 *
 * The ESP32-C6 co-processor handles BOTH BLE controller AND WiFi over a
 * single SDIO link via esp_hosted. Running BLE-discovery (which keeps a
 * 5 s scanning window open and emits HCI traffic the whole time) at the
 * same time as a WiFi STA scan + WiFi mode switch tends to overload the
 * slave: HCI ACKs time out, then NimBLE auto-resets, then RPC for the
 * WiFi calls also times out, and a few resets later the host crashes
 * with a Load-Access-Fault.
 *
 * wifi_mgr calls obd_bt_pause(true) before scanning / mode-switching and
 * obd_bt_pause(false) afterwards. While paused:
 *   - any in-flight BLE scan is cancelled
 *   - new scans don't start (state stays IDLE)
 *   - existing connections are kept (the controller is still talked to,
 *     but only on the connection's link, no broadcast scanning)
 */
void       obd_bt_pause(bool paused);

#ifdef __cplusplus
}
#endif
