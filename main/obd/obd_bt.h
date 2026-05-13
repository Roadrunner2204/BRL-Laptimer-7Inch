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
    OBD_FINGERPRINTING,   // post-subscribe, reading VIN for the cache key
    OBD_DISCOVERING,      // post-VIN, reading the supported-PIDs bitmap
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

/**
 * Access to the cached Universal-OBD2 profile for the dashboard slot
 * picker. Returns an opaque pointer (cast to `const CarProfile *` —
 * we don't include car_profile.h here to keep this header free of
 * C++/extern-"C" entanglement). NULL until the Universal-Liste was
 * loaded once on the first OBD-BT connect. The picker walks
 * `sensors[0..sensor_count-1]` and lets the user assign sensor
 * index N to a Z3 slot via slot ID `128 + N` (see dash_config.h
 * slot ranges).
 */
const void *obd_bt_pid_profile(void);

/**
 * Same for the active vehicle profile (e.g. /cars/N47F.brl) — sourced
 * from car_profile_get_active() at every reconnect. Carries proto=2
 * BMW UDS DIDs that the Universal-OBD2-Liste doesn't have, plus any
 * vehicle-specific extra Mode-01 PIDs. NULL when no active profile is
 * configured or the file isn't on the SD.
 */
const void *obd_bt_vehicle_profile(void);

/**
 * Look up the most recent live value for a CarSensor (matched by
 * pointer identity against the active sensor list). Used by the
 * dashboard slot renderer for dynamic slots (IDs 128..255).
 *
 *   sensor    — pointer into a CarProfile.sensors[] array
 *   out_value — receives the post-scale/offset value
 *   out_age_ms — optional, receives ms since the last successful decode
 *
 * Returns true when a value has ever been received this session, false
 * when the sensor hasn't answered yet (caller renders "--"). The value
 * stays available until the next disconnect (last-known-good even after
 * the ECU goes silent) — this is what makes the dashboard slots stop
 * flickering when the BMW responds sporadically to a particular DID.
 *
 * Opaque pointer return-types in the rest of this header keep
 * car_profile.h out of the include chain; here we accept const void *
 * (cast from const CarSensor *) for the same reason.
 */
bool obd_bt_get_sensor_value(const void *sensor,
                             float       *out_value,
                             uint32_t    *out_age_ms);

/**
 * Mode-01 PID-Discovery (CMD_DISCOVER_PIDS = 0x06 im Adapter ab v1.1).
 *
 * Beim BLE-Connect fragt der Adapter intern die Mode-01 PIDs 0x00, 0x20,
 * 0x40, 0x60, 0x80, 0xA0, 0xC0 ab — jede Antwort ist eine 4-byte Bitmap
 * der nächsten 32 PIDs die das Auto unterstützt. Der Adapter kombiniert
 * das zu einer 28-byte Gesamt-Bitmap und schickt sie ans Display.
 *
 * Das Display nutzt diese Bitmap um:
 *   1. nicht-supportete PIDs gar nicht erst zu pollen (wie Torque Pro)
 *   2. den Sensor-Picker auf nur tatsächlich verfügbare PIDs zu filtern
 *
 * obd_pid_is_supported(pid) gibt true zurück wenn die DISCOVER-Bitmap
 * das Bit für diesen PID gesetzt hat. Vor dem ersten erfolgreichen
 * Connect (oder wenn der Adapter die Discovery nicht beantwortet) gibt
 * die Funktion immer true zurück — dann fällt das System auf das
 * traditionelle Try-and-Cache-Verhalten zurück.
 *
 * obd_pid_discovery_complete() sagt ob die Bitmap überhaupt vorliegt.
 */
bool obd_pid_is_supported(uint8_t pid);
bool obd_pid_discovery_complete(void);

#ifdef __cplusplus
}
#endif
