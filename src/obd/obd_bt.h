#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * obd_bt — BLE client for BRL OBD Adapter
 *
 * Connection model:
 *   - Scans for device named "BRL-OBD"
 *   - Connects via Nordic UART Service (NUS)
 *   - Sends ELM327-like AT/OBD commands: "01 0C\r" (RPM), etc.
 *   - Parses responses, writes to g_state.obd
 *
 * State machine:
 *   IDLE → SCANNING → FOUND → CONNECTING → CONNECTED → REQUESTING → CONNECTED
 *                                                          (loop)
 *   Any error → IDLE (auto-retry after 5 s)
 *
 * Custom BRL PIDs (mode 22 / extended):
 *   0xA0 = brake pedal %      (0–100)
 *   0xA1 = steering angle °   (signed, ×10 encoding)
 *
 * Standard PIDs used (mode 01):
 *   0x04 = engine load %
 *   0x05 = coolant temp °C    (A − 40)
 *   0x0B = MAP kPa            (A)
 *   0x0C = RPM                ((256A + B) / 4)
 *   0x0F = intake air temp °C (A − 40)
 *   0x11 = throttle pos %     (A × 100 / 255)
 *   0x24 = O2 lambda wide     (word)
 */

typedef enum {
    OBD_IDLE = 0,
    OBD_SCANNING,
    OBD_FOUND,
    OBD_CONNECTING,
    OBD_CONNECTED,
    OBD_REQUESTING,
    OBD_ERROR
} OBdBtState;

void     obd_bt_init();
void     obd_bt_poll();    // call in loop() — non-blocking
OBdBtState obd_bt_state();
void     obd_bt_disconnect();
