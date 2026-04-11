#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * can_bus -- Direct CAN bus interface via SN65HVD230 transceiver
 *
 * Uses the ESP32-P4 TWAI (Two-Wire Automotive Interface) peripheral
 * connected to an SN65HVD230 CAN transceiver module.
 *
 * Pin assignment (directly adjacent to GPS header pins):
 *   CAN TX = GPIO 23  (ESP32 -> SN65HVD230 D pin)
 *   CAN RX = GPIO 24  (SN65HVD230 R pin -> ESP32)
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

#define CAN_TX_PIN  5       // ESP32 -> SN65HVD230 D pin  (Header IO5)
#define CAN_RX_PIN  28      // SN65HVD230 R pin -> ESP32  (Header IO28)

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
