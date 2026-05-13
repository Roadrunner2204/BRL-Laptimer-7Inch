#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * can_bus -- Direct CAN bus interface via the on-board TJA1051T/3/1J
 *
 * Uses the ESP32-P4 TWAI (Two-Wire Automotive Interface) peripheral
 * wired to the TJA1051 CAN transceiver that's already populated on the
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (no external module required).
 * CANH/CANL come out on the on-board CAN header.
 *
 * Pin assignment (per Waveshare schematic, fixed by board layout):
 *   CAN TX = GPIO 22  (ESP32 -> TJA1051 TXD)
 *   CAN RX = GPIO 21  (TJA1051 RXD -> ESP32)
 *
 * The active car profile (.brl) defines which CAN IDs to listen to,
 * how to extract signals (byte position, length, scale/offset), and
 * updates g_state.obd with decoded values.
 *
 * Usage:
 *   can_bus_init()  -- install TWAI driver at configured bitrate
 *   can_bus_poll()  -- call in logic loop, processes received frames
 *   can_bus_stop()  -- uninstall TWAI driver
 */

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_TX_PIN  22      // ESP32 -> TJA1051 TXD  (on-board)
#define CAN_RX_PIN  21      // TJA1051 RXD -> ESP32  (on-board)

/// Initialize and start TWAI driver using bitrate from active car profile.
/// Returns true if driver started successfully.
bool can_bus_init(void);

/// Process received CAN frames and update g_state.obd.
/// Non-blocking; call in logic loop (~5ms interval).
void can_bus_poll(void);

/// Stop TWAI driver and release resources.
void can_bus_stop(void);

/// Returns true if TWAI driver is running and receiving frames.
bool can_bus_active(void);

#ifdef __cplusplus
}
#endif
