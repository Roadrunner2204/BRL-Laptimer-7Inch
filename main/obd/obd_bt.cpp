/**
 * obd_bt.cpp -- BLE client for BRL OBD Adapter (ESP-IDF NimBLE host API)
 *
 * Replaces NimBLE-Arduino with the ESP-IDF NimBLE host stack.
 * On ESP32-P4 the NimBLE host runs on the P4 while the BLE controller
 * runs on the ESP32-C6 co-processor (connected via SDIO / esp_hosted).
 * The Waveshare BSP configures the host-controller transport.
 *
 * Target  : "BRL OBD Adapter"
 * Our name: "BRL-Laptimer"
 *
 * Service  : 0000FFE0-0000-1000-8000-00805F9B34FB
 * CMD ch.  : 0000FFE1  Write  (Laptimer -> Adapter)
 * RESP ch. : 0000FFE2  Notify (Adapter -> Laptimer)
 *
 * Binary protocol
 *   Send : [OBDCmd 1B] [payload...]
 *   Recv : [OBDCmd 1B] [OBDStatus 1B] [data bytes...]
 *
 * OBD-II mode 01 PIDs requested:
 *   0x0C RPM, 0x11 Throttle, 0x0B MAP, 0x05 Coolant, 0x0F Intake
 */

#include "obd_bt.h"
#include "obd_status.h"
#include "obd_pid_cache.h"
#include "obd_dynamic.h"
#include "../data/lap_data.h"
#include "../data/car_profile.h"
#include "../ui/dash_config.h"
#include "compat.h"

#include <string.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "obd_bt";

// ---------------------------------------------------------------------------
// UUIDs  (Bluetooth base UUID with 16-bit short IDs 0xFFE0, 0xFFE1, 0xFFE2)
// ---------------------------------------------------------------------------
static const ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xE0, 0xFF, 0x00, 0x00
);
static const ble_uuid128_t CMD_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xE1, 0xFF, 0x00, 0x00
);
static const ble_uuid128_t RESP_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xE2, 0xFF, 0x00, 0x00
);

#define TARGET_NAME      "BRL OBD Adapter"
#define SCAN_DURATION_MS 5000
#define RETRY_INTERVAL   5000   // ms between reconnect attempts
#define REQ_TIMEOUT_MS   1000   // ms to wait for a PID response

// ---------------------------------------------------------------------------
// Binary protocol enums
// ---------------------------------------------------------------------------
enum OBDCmd : uint8_t {
    CMD_READ_PID       = 0x01,   // [PID]                  → [SVC, PID, data...]
    CMD_READ_VIN       = 0x02,   // []                     → [VIN 17 chars]
    CMD_READ_MULTI_PID = 0x05,   // [PID1..PID6, max 6]    → [SVC, PID1, d.., PID2, d..]
    CMD_PING           = 0xFF,
};
enum OBDStatus : uint8_t {
    STATUS_OK       = 0x00,
    STATUS_TIMEOUT  = 0x01,
    STATUS_NO_RESP  = 0x02,
    STATUS_NEGATIVE = 0x03,
    STATUS_BUS_ERR  = 0x04,
    STATUS_NOT_INIT = 0x05,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static OBdBtState    s_state       = OBD_IDLE;
static uint16_t      s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t      s_cmd_val_handle  = 0;   // ATT handle for CMD char value
static uint16_t      s_resp_val_handle = 0;   // ATT handle for RESP char value
static uint16_t      s_resp_cccd_handle = 0;  // ATT handle for RESP CCCD
static uint32_t      s_retry_ts    = 0;
static uint32_t      s_req_ts      = 0;
static uint8_t       s_own_addr_type = 0;

// Peer address found during scan
static ble_addr_t    s_peer_addr;

// Service discovery bookkeeping
static bool          s_svc_found   = false;
static uint16_t      s_svc_start   = 0;
static uint16_t      s_svc_end     = 0;

// Response buffer -- written from NimBLE callback, read from poll().
// Sized for the worst-case multi-PID response: [CMD][STATUS][SVC=0x41]
// + 6 × (1 byte PID echo + up to 4 byte data) = 3 + 30 = 33 bytes. We
// round up to 64 to leave slack for any future commands and to keep the
// buffer aligned. (32 was the single-PID limit and would truncate the
// last PID of a 6×4-byte multi-response.)
static uint8_t       s_rx_buf[64]  = {};
static uint8_t       s_rx_len      = 0;
static volatile bool s_rx_ready    = false;

// NimBLE host sync flag
static volatile bool s_ble_synced  = false;

// Pause-flag set by wifi_mgr during scans / mode switches. While paused
// we do not start new BLE scans and we cancel any in-flight one. Active
// connections are NOT torn down — the controller still services the link.
static volatile bool s_paused = false;

// Crash-storm protection. esp_hosted can drop into a state where every
// HCI request times out and NimBLE keeps resetting; a runaway reset
// loop is what crashed Core 0 in the field. Count resets in a sliding
// window; if we hit RESET_STORM_LIMIT in RESET_STORM_WINDOW_MS, force
// ourselves into a long IDLE before trying anything BLE again.
#define RESET_STORM_WINDOW_MS  10000
#define RESET_STORM_LIMIT      3
#define RESET_STORM_COOLDOWN   30000
static uint32_t s_reset_log[RESET_STORM_LIMIT] = {};
static uint8_t  s_reset_idx                    = 0;
static uint32_t s_storm_until                  = 0;

// ---------------------------------------------------------------------------
// PID round-robin
//
// Default list (used when no car profile is loaded). Mode 01 PIDs decoded
// with the hardcoded formulas in apply_default_pid() below.
static const uint8_t DEFAULT_PIDS[] = {
    0x0C,  // RPM          (2 B) -> (256A + B) / 4
    0x11,  // Throttle     (1 B) -> A * 100 / 255
    0x0B,  // MAP kPa      (1 B) -> A
    0x05,  // Coolant C    (1 B) -> A - 40
    0x0F,  // Intake C     (1 B) -> A - 40
};

// Active PID entry. When sensor != nullptr, the response is decoded with
// the profile's scale/offset/start/len instead of the built-in formulas.
//
// Dead-PID tracking: when an ECU answers STATUS_TIMEOUT or NEGATIVE
// repeatedly for the same PID, it doesn't support it — keep polling
// it would just waste round-trip time. After DEAD_THRESHOLD strikes
// the PID is skipped in the round-robin. Every REVIVE_INTERVAL_MS we
// flip every dead PID back alive once for a re-test, in case the
// ECU started supporting it (engine warmed up, ABS module woke etc.).
//
// `unmapped_logged` suppresses the "no dashboard slot routes that name"
// warning after the first hit per PID — otherwise it spams every cycle.
struct ActivePid {
    uint8_t          pid;
    const CarSensor *sensor;
    uint8_t          strikes;          // consecutive bad responses
    bool             dead;
    bool             unmapped_logged;  // route_sensor warning fired once
};
#define MAX_ACTIVE_PIDS    32
#define DEAD_THRESHOLD     3
#define REVIVE_INTERVAL_MS 30000
// Adapter firmware accepts up to 6 PIDs per request, but BMW E-series ECUs
// (DDE/MSD80) frequently drop one or two answers when 6 are bundled — we
// then strike valid PIDs as missing and they end up dead even though they
// work fine. 4 PIDs per request is the sweet spot that BMW reliably
// returns; only ~50 ms slower than 6 per round-trip.
#define MULTI_PID_MAX      4
static ActivePid s_pids[MAX_ACTIVE_PIDS];
static uint8_t   s_pid_count   = 0;
static uint8_t   s_pid_idx     = 0;
static uint32_t  s_last_revive_ms = 0;

// PIDs sent in the current outstanding multi-request. Used to attribute
// strikes on timeout / partial response, and to advance s_pid_idx past
// the served block so the next round picks up where we left off.
static uint8_t   s_inflight_pids[MULTI_PID_MAX] = {};
static uint8_t   s_inflight_count               = 0;

// Cache-poisoning guard. We persist a PID as DEAD only after the session
// has seen at least one *real* response come back. Without this, a stuck
// CAN bus or a transient adapter problem during the first 3 round-trips
// could write every PID as dead into NVS — and the next connect would
// then start with 0 alive PIDs, even though the car is healthy. Once a
// single STATUS_OK lands, we know the link is sane and DEAD writes are
// trustworthy.
static bool      s_session_had_alive             = false;

// Secondary profile loaded from /cars/OBD.brl. Only populated when the main
// car profile doesn't contain OBD2 (proto=7DF) sensors, so that users with
// a PT-CAN hardwire profile still get OBD2 data via the BLE adapter.
static CarProfile s_obd_profile_fallback = {};
static bool       s_obd_profile_tried    = false;

static int count_obd2_sensors(const CarProfile *p)
{
    if (!p || !p->loaded) return 0;
    int n = 0;
    for (int i = 0; i < p->sensor_count; i++) {
        if (p->sensors[i].proto == 7) n++;
    }
    return n;
}

static void append_obd2_sensors(const CarProfile *p)
{
    if (!p || !p->loaded) return;
    for (int i = 0; i < p->sensor_count; i++) {
        const CarSensor *s = &p->sensors[i];
        if (s->proto != 7) continue;
        if (s_pid_count >= MAX_ACTIVE_PIDS) break;
        s_pids[s_pid_count].pid    = (uint8_t)(s->can_id & 0xFF);
        s_pids[s_pid_count].sensor = s;
        s_pids[s_pid_count].strikes         = 0;
        s_pids[s_pid_count].dead            = false;
        s_pids[s_pid_count].unmapped_logged = false;
        // Diagnostic — log each sensor's .brl decode parameters. If RPM
        // shows wrong values in the field, comparing this dump to the
        // OBD2 spec (start=2, len=2, scale=0.25) immediately reveals
        // whether the .brl was generated with PT-CAN scaling by mistake.
        ESP_LOGI(TAG, "  PID 0x%02X '%s'  start=%d len=%d scale=%.6f "
                      "offset=%.3f  unsigned=%d",
                 (unsigned)(s->can_id & 0xFFu),
                 s->name,
                 (int)s->start, (int)s->len,
                 (double)s->scale, (double)s->offset,
                 (int)s->is_unsigned);
        s_pid_count++;
    }
}

// Build the active PID list. The BRL OBD BLE Adapter speaks OBD2 Mode 01
// and only that, so the data source is always /cars/OBD.brl — NOT the
// currently-active vehicle profile (which lives alongside for CAN-Direct
// hardwire use and display labelling).
//
// Order of preference:
//   1. /cars/OBD.brl from SD card (loaded lazily on first use).
//   2. Built-in DEFAULT_PIDS (RPM / TPS / MAP / Coolant / Intake) as a
//      last resort if OBD.brl isn't on the card.
//
// "OBD2 sensor" inside OBD.brl means CAN-Checked proto field "7DF"
// (atoi() gives 7). Each sensor's low can_id byte is taken as the Mode-01
// PID. Non-OBD2 protocols (PT-CAN broadcast=0, BMW UDS=1, 29-bit extended
// etc.) in OBD.brl are skipped because the adapter firmware only handles
// Mode 01.
// `cache_key` identifies the connected vehicle for the PID cache. The
// caller picks it — preferred is the ECU fingerprint from PID 0x00
// (ISO-15031-5 supported-PIDs bitmap, unique per ECU model), falling
// back to the active car-profile name when fingerprinting fails.
static void rebuild_pid_list(const char *cache_key)
{
    s_pid_count = 0;
    s_pid_idx   = 0;

    obd_pid_cache_load(cache_key);

    // Load /cars/OBD.brl lazily — once per session. If the user adds or
    // replaces OBD.brl, a reboot (or a physical BLE reconnect, which forces
    // the tried-flag below back to false in future) picks it up.
    if (!s_obd_profile_tried) {
        s_obd_profile_tried = true;
        if (car_profile_load_into("OBD.brl", &s_obd_profile_fallback)) {
            ESP_LOGI(TAG, "Loaded /cars/OBD.brl: %d sensors (%d OBD2-flagged)",
                     s_obd_profile_fallback.sensor_count,
                     count_obd2_sensors(&s_obd_profile_fallback));
        } else {
            ESP_LOGW(TAG, "/cars/OBD.brl not found — using built-in 5-PID fallback");
        }
    }

    if (count_obd2_sensors(&s_obd_profile_fallback) > 0) {
        append_obd2_sensors(&s_obd_profile_fallback);
        ESP_LOGI(TAG, "OBD.brl: %d OBD2 PIDs active for BLE adapter",
                 s_pid_count);
    } else {
        // Last resort — hardcoded defaults (RPM/TPS/MAP/Coolant/Intake)
        for (uint8_t p : DEFAULT_PIDS) {
            s_pids[s_pid_count].pid    = p;
            s_pids[s_pid_count].sensor = nullptr;
            s_pids[s_pid_count].strikes         = 0;
            s_pids[s_pid_count].dead            = false;
            s_pids[s_pid_count].unmapped_logged = false;
            s_pid_count++;
        }
        ESP_LOGI(TAG, "Using %d built-in OBD2 PIDs (no OBD.brl on SD)",
                 s_pid_count);
    }

    // Apply the cache as a *hint* rather than gospel. Earlier sessions
    // could have been running with a buggy decoder (off-by-one start
    // offset, wrong scaling) that mis-marked perfectly working PIDs as
    // dead. We mustn't let those past mistakes lock the user out of
    // valid sensors forever.
    //
    // New strategy: cache-dead PIDs start *alive* but with `strikes` set
    // to DEAD_THRESHOLD-1 — i.e. one bad response away from being dead
    // again. So:
    //   - if the PID really is unsupported by this car, the very first
    //     no-answer flips it dead and we save the round-trip on every
    //     subsequent cycle (cache optimisation kept).
    //   - if the PID actually works (e.g. earlier dead-marking was a
    //     bug), it answers, strikes reset to 0, alive sticks, cache
    //     is corrected on the next save.
    int seeded_hint = 0;
    for (int i = 0; i < s_pid_count; i++) {
        if (obd_pid_cache_is_dead(s_pids[i].pid)) {
            s_pids[i].dead    = false;          // start alive
            s_pids[i].strikes = DEAD_THRESHOLD - 1;  // but on probation
            seeded_hint++;
        }
    }
    if (seeded_hint > 0) {
        ESP_LOGI(TAG, "PID cache: %d previously-dead PIDs on probation "
                      "(will retry once before re-marking dead)",
                 seeded_hint);
    }
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_scan(void);
static bool note_ble_reset(uint32_t now);
static void maybe_revive_dead_pids(uint32_t now);

// ---------------------------------------------------------------------------
// Route a decoded sensor value into g_state.obd by sensor name.
//
// Only names that map to a g_state.obd field are routed — other values are
// currently dropped because there's no generic slot for them. Extending this
// (arbitrary sensor -> arbitrary dashboard slot) is separate work.
// ---------------------------------------------------------------------------
// Match either an exact name or a substring. CAN-Checked profiles in the
// wild have wildly different conventions ("RPM" / "Drehzahl" / "Engine_RPM" /
// "EngineSpeed"), so we accept aliases generously.
static bool name_contains(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != nullptr;
}

// Returns true if the value was actually routed somewhere — used by the
// caller to log unmapped names.
static bool route_sensor(const char *name, float value)
{
    if (!name || name[0] == '\0') return false;
    ObdData &obd = g_state.obd;

    // RPM aliases
    if (strcmp(name, "RPM") == 0
        || name_contains(name, "Drehzahl")
        || name_contains(name, "EngineSpeed")
        || name_contains(name, "Engine_RPM")
        || name_contains(name, "Engine RPM")) {
        obd.rpm = value;
        obd_status_mark_active(FIELD_RPM);
        return true;
    }

    // Throttle aliases
    if (strcmp(name, "TPS") == 0
        || strcmp(name, "Throttle") == 0
        || name_contains(name, "Drossel")
        || name_contains(name, "Throttle")) {
        obd.throttle_pct = value;
        obd_status_mark_active(FIELD_THROTTLE);
        return true;
    }

    // Boost / MAP aliases
    if (strcmp(name, "Boost") == 0 || strcmp(name, "MAP") == 0
        || name_contains(name, "Saugrohr")
        || name_contains(name, "Manifold")
        || name_contains(name, "Boost")
        || name_contains(name, "Ladedruck")) {
        obd.boost_kpa = value;
        obd_status_mark_active(FIELD_BOOST);
        return true;
    }

    // Lambda
    if (strcmp(name, "Lambda") == 0
        || name_contains(name, "Lambda")
        || name_contains(name, "AFR")) {
        obd.lambda = value;
        obd_status_mark_active(FIELD_LAMBDA);
        return true;
    }

    // Coolant temp
    if (strcmp(name, "WaterT") == 0 || strcmp(name, "CoolantT") == 0
        || strcmp(name, "Coolant") == 0
        || name_contains(name, "Kühlwasser")
        || name_contains(name, "Kuehlwasser")
        || name_contains(name, "Coolant")
        || name_contains(name, "WaterTemp")) {
        obd.coolant_temp_c = value;
        obd_status_mark_active(FIELD_COOLANT);
        return true;
    }

    // Intake temp
    if (strcmp(name, "IntakeT") == 0 || strcmp(name, "IAT") == 0
        || name_contains(name, "Ansaug")
        || name_contains(name, "Intake")
        || name_contains(name, "AirTemp")) {
        obd.intake_temp_c = value;
        obd_status_mark_active(FIELD_INTAKE);
        return true;
    }

    // Brake
    if (strcmp(name, "Brake") == 0
        || name_contains(name, "Bremse")
        || name_contains(name, "Brake")) {
        obd.brake_pct = value;
        obd_status_mark_active(FIELD_BRAKE);
        return true;
    }

    // Steering
    if (strcmp(name, "Steering") == 0
        || name_contains(name, "Lenk")
        || name_contains(name, "Steering")) {
        obd.steering_angle = value;
        obd_status_mark_active(FIELD_STEERING);
        return true;
    }

    // Battery voltage (PID 0x42 — Control Module Voltage)
    if (strcmp(name, "BattVolt") == 0
        || name_contains(name, "Battery")
        || name_contains(name, "Batterie")
        || name_contains(name, "BattV")
        || name_contains(name, "Bordspannung")
        || name_contains(name, "Bordnetz")) {
        obd.battery_v = value;
        obd_status_mark_active(FIELD_BATTERY);
        return true;
    }

    // Mass Air Flow (PID 0x10)
    if (strcmp(name, "MAF") == 0
        || name_contains(name, "MAF")
        || name_contains(name, "AirMass")
        || name_contains(name, "Luftmasse")) {
        obd.maf_gps = value;
        obd_status_mark_active(FIELD_MAF);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Decode response via profile sensor (scale/offset/start/len from the .brl).
// Used when a car profile is active and the PID belongs to it.
// ---------------------------------------------------------------------------
// Returns true if the value was routed into a g_state.obd.* slot, false
// if the sensor name doesn't match any alias.
//
// Buffer convention: `d` points at the first DATA byte of the OBD2 response
// (i.e. [SVC=0x41][PID-echo] have been stripped by the multi-PID demuxer).
// CAN-Checked .brl files however define `s->start` from the start of the
// **CAN frame**, which on a Mode-01 single-frame ISO-TP response looks like:
//   byte 0: PCI (single-frame length nibble)
//   byte 1: SVC = 0x41
//   byte 2: PID-echo
//   byte 3: D1     ← typical RPM/WaterT/etc. start in the .brl
//   byte 4: D2
// So .brl `start=3` corresponds to data offset 0. We subtract 3 to map.
// Off-by-one here was the cause of "RPM 10-42" (decoder read D2 alone)
// and silent drop of 1-byte PIDs (off >= len triggered the early exit).
// ---------------------------------------------------------------------------
static bool apply_profile_sensor(const CarSensor *s,
                                 const uint8_t *d, uint8_t len)
{
    if (!s) return false;
    int32_t off = (int32_t)s->start - 3;   // strip [PCI][0x41][PID] header
    if (off < 0) off = 0;                  // sensors with 0-based start still work
    if (off >= len) return false;
    int32_t raw = 0;
    if (s->len == 2 && off + 1 < len) {
        // Big-endian (Motorola byte order, what OBD2 returns)
        raw = ((int32_t)d[off] << 8) | d[off + 1];
    } else {
        raw = d[off];
    }
    if (!s->is_unsigned) {
        if (s->len == 2 && raw > 32767) raw -= 65536;
        else if (s->len == 1 && raw > 127) raw -= 256;
    }
    float value = (float)raw * s->scale + s->offset;
    if (value < s->min_val) value = s->min_val;
    if (value > s->max_val) value = s->max_val;
    // Stash the post-scale value in the per-PID dynamic cache so any
    // dashboard slot mapped to this OBD.brl sensor can read it back —
    // independent of whether route_sensor() recognises the name as one
    // of the legacy hardcoded fields. This is what makes "all OBD2
    // sensors selectable in the picker" work.
    obd_dynamic_set((uint8_t)(s->can_id & 0xFFu), value);
    return route_sensor(s->name, value);
}

// ---------------------------------------------------------------------------
// Parse response and update g_state.obd.
//
// If the current PID came from a car profile entry, decode via the profile's
// scale/offset. Otherwise use the built-in formulas for the 5 default PIDs.
// Returns true if the value was actually routed somewhere — false means the
// PID decoded but the sensor's name has no matching slot (caller logs once).
// ---------------------------------------------------------------------------
static bool apply_pid(uint8_t pid, const CarSensor *sensor,
                      const uint8_t *d, uint8_t len)
{
    if (sensor) {
        return apply_profile_sensor(sensor, d, len);
    }
    // No sensor mapping — built-in fallback decoder for the 5 hardcoded
    // PIDs (used when /cars/OBD.brl is absent). Mirror the value into
    // obd_dynamic so dynamic slots still work in this minimal mode.
    ObdData &obd = g_state.obd;
    float v = 0.0f; bool ok = false;
    switch (pid) {
        case 0x0C: if (len >= 2) { v = ((d[0] * 256u) + d[1]) / 4.0f; obd.rpm = v;
                                   obd_status_mark_active(FIELD_RPM); ok = true; } break;
        case 0x11: if (len >= 1) { v = d[0] * 100.0f / 255.0f; obd.throttle_pct = v;
                                   obd_status_mark_active(FIELD_THROTTLE); ok = true; } break;
        case 0x0B: if (len >= 1) { v = (float)d[0]; obd.boost_kpa = v;
                                   obd_status_mark_active(FIELD_BOOST); ok = true; } break;
        case 0x05: if (len >= 1) { v = (float)d[0] - 40.0f; obd.coolant_temp_c = v;
                                   obd_status_mark_active(FIELD_COOLANT); ok = true; } break;
        case 0x0F: if (len >= 1) { v = (float)d[0] - 40.0f; obd.intake_temp_c = v;
                                   obd_status_mark_active(FIELD_INTAKE); ok = true; } break;
        default: break;
    }
    if (ok) obd_dynamic_set(pid, v);
    return ok;
}

// ---------------------------------------------------------------------------
// Round-robin helpers — skip dead PIDs so we don't waste adapter round-trips
// on things the ECU never answers, and bundle up to MULTI_PID_MAX live PIDs
// per request so one ~80 ms BLE round-trip serves 6 dashboard slots.
// ---------------------------------------------------------------------------
static ActivePid *find_active_pid(uint8_t pid)
{
    for (int i = 0; i < s_pid_count; i++) {
        if (s_pids[i].pid == pid) return &s_pids[i];
    }
    return nullptr;
}

// Walk forward from s_pid_idx and pack up to MULTI_PID_MAX alive PIDs into
// s_inflight_pids[]. Advances s_pid_idx past the last gathered slot so the
// next call continues round-robin where this one stopped. If every PID is
// dead, returns 0 — caller should idle and let maybe_revive_dead_pids() try.
static uint8_t gather_inflight(void)
{
    s_inflight_count = 0;
    if (s_pid_count == 0) return 0;
    int last_picked = -1;
    for (int tries = 0;
         tries < s_pid_count && s_inflight_count < MULTI_PID_MAX;
         tries++) {
        int idx = (s_pid_idx + tries) % s_pid_count;
        if (!s_pids[idx].dead) {
            s_inflight_pids[s_inflight_count++] = s_pids[idx].pid;
            last_picked = idx;
        }
    }
    if (last_picked >= 0) {
        s_pid_idx = (last_picked + 1) % s_pid_count;
    }
    return s_inflight_count;
}

static void maybe_revive_dead_pids(uint32_t now)
{
    if (s_last_revive_ms == 0) s_last_revive_ms = now;
    if (now - s_last_revive_ms < REVIVE_INTERVAL_MS) return;
    s_last_revive_ms = now;
    int revived = 0;
    for (int i = 0; i < s_pid_count; i++) {
        // Don't waste cycles on PIDs that the cache says are confirmed
        // dead for this car — those only come back via an explicit
        // cache clear in Settings or a different car profile.
        if (s_pids[i].dead && !obd_pid_cache_is_dead(s_pids[i].pid)) {
            s_pids[i].dead    = false;
            s_pids[i].strikes = 0;
            revived++;
        }
    }
    if (revived > 0) {
        ESP_LOGI(TAG, "Reviving %d dead PIDs for re-test", revived);
    }
    // Opportunistic save: any dead-flag changes from the last 30 s land
    // in NVS now. Cheap when nothing's dirty.
    obd_pid_cache_save_if_dirty();
}

// Send a multi-PID request — up to 6 PIDs in a single round-trip.
// Adapter answers `[CMD][STATUS][SVC=41][PID1][data1..][PID2][data2..]...`
// in one notification. With 6 PIDs per ~80 ms round-trip this beats
// Torque Pro's typical ELM327 throughput by a wide margin.
static void send_multi_pid_request(const uint8_t *pids, uint8_t count)
{
    if (s_cmd_val_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (count == 0) return;
    if (count > 6) count = 6;
    uint8_t cmd[1 + 6];
    cmd[0] = CMD_READ_MULTI_PID;
    for (uint8_t i = 0; i < count; i++) cmd[1 + i] = pids[i];
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_cmd_val_handle,
                                         cmd, 1 + count);
    if (rc != 0) {
        ESP_LOGW(TAG, "Write MULTI_PID failed: rc=%d", rc);
        return;
    }
    s_rx_ready = false;
    s_req_ts   = millis();
    s_state    = OBD_REQUESTING;
}

// Send a VIN read for the ECU fingerprint. Adapter does the Mode 09
// PID 02 ISO-TP multi-frame dance internally and returns the 17-char
// VIN as a single payload. Used during OBD_FINGERPRINTING.
static void send_vin_request(void)
{
    if (s_cmd_val_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t cmd[1] = { CMD_READ_VIN };
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_cmd_val_handle,
                                         cmd, sizeof(cmd));
    if (rc != 0) {
        ESP_LOGW(TAG, "Write VIN failed: rc=%d", rc);
        return;
    }
    s_rx_ready = false;
    s_req_ts   = millis();
}

// OBD2 standard data-length per PID, used to demultiplex the
// MULTI_PID response. `len` from the active CarSensor takes
// precedence — the adapter just glues the raw bus bytes together,
// it doesn't re-frame, so we must know each PID's payload size.
static uint8_t obd2_standard_pid_len(uint8_t pid)
{
    switch (pid) {
        // 2-byte PIDs
        case 0x02: case 0x03: case 0x0C: case 0x10:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1F: case 0x21: case 0x22: case 0x23:
        case 0x31: case 0x3C: case 0x3D: case 0x3E:
        case 0x3F: case 0x42: case 0x43: case 0x44:
        case 0x4D: case 0x4E: case 0x50:
        case 0x5D: case 0x5E:
            return 2;
        // 4-byte (wide-range O2 sensors)
        case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            return 4;
        // PID 0x00, 0x20, 0x40, 0x60... (supported-PIDs bitmaps)
        case 0x00: case 0x20: case 0x40:
        case 0x60: case 0x80: case 0xA0: case 0xC0:
            return 4;
        // Default: 1 byte (most temperatures, percentages, single bytes)
        default:
            return 1;
    }
}

// ---------------------------------------------------------------------------
// Disconnect helper
// ---------------------------------------------------------------------------
static void do_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    s_cmd_val_handle   = 0;
    s_resp_val_handle  = 0;
    s_resp_cccd_handle = 0;
    s_svc_found = false;
    g_state.obd.connected = false;
    g_state.obd_connected = false;
    // Allow OBD.brl to be re-tried on the next connect attempt — if the
    // SD card wasn't ready during the very first try (boot race), this
    // is what gets the PID list populated correctly the second time.
    s_obd_profile_tried = false;
    // Persist whatever we learned this session. Survives reboot so the
    // next connect skips the dead PIDs from frame 1.
    obd_pid_cache_save_if_dirty();
    // New connection = new permission to learn. Cache-poisoning guard
    // resets so the next session must again see ≥1 STATUS_OK before
    // any DEAD writes start hitting NVS.
    s_session_had_alive = false;
    s_inflight_count    = 0;
    // Wipe per-PID live cache so the next session doesn't briefly show
    // stale values from the previous car/connection.
    obd_dynamic_clear();
}

// ---------------------------------------------------------------------------
// Enable notifications on RESP characteristic by writing 0x0001 to its CCCD
// ---------------------------------------------------------------------------
static int on_subscribe_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to RESP notifications");
        g_state.obd.connected = true;
        g_state.obd_connected = true;
        // Probe the ECU for its supported-PIDs bitmap (Mode 01 PID 0x00).
        // Every OBD2-conformant car answers this with 4 bytes encoding
        // which of PIDs 0x01..0x20 it supports — that's effectively a
        // unique fingerprint for the model+ECU combination, much more
        // specific than the user-picked car-profile name. We use the
        // hash as the PID-cache key so two cars sharing a profile
        // (e. g. driver tries the same .brl on his M3 and his RS3) get
        // their own cache entries automatically.
        ESP_LOGI(TAG, "Connected — reading VIN for cache fingerprint");
        // The BRL OBD Adapter exposes Mode 09 PID 02 (VIN) via the
        // CMD_READ_VIN binary command (it does the ISO-TP multi-frame
        // assembly internally and returns just the 17 chars). The VIN
        // is the cleanest possible per-vehicle key — uniquely identifies
        // the car, no risk of profile-name collisions across two
        // physical cars sharing the same .brl.
        send_vin_request();
        s_state = OBD_FINGERPRINTING;
    } else {
        ESP_LOGE(TAG, "Subscribe failed: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
    }
    return 0;
}

static void subscribe_notifications(void)
{
    if (s_resp_cccd_handle == 0) {
        ESP_LOGE(TAG, "CCCD handle not found, cannot subscribe");
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return;
    }

    uint8_t val[2] = { 0x01, 0x00 };  // enable notifications
    int rc = ble_gattc_write_flat(s_conn_handle, s_resp_cccd_handle,
                                  val, sizeof(val), on_subscribe_cb, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD write failed: rc=%d", rc);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
    }
}

// ---------------------------------------------------------------------------
// GATT descriptor discovery callback -- locates the CCCD for RESP
// ---------------------------------------------------------------------------
static int gatt_dsc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        // All descriptors discovered -- now subscribe
        subscribe_notifications();
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Descriptor disc error: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return 0;
    }

    // Check if this descriptor is the CCCD (UUID 0x2902).
    //
    // NimBLE's `ble_gattc_disc_all_dscs(start=chr_val_handle, end=service_end)`
    // can return multiple 0x2902 descriptors when the search range bleeds
    // past RESP into a following characteristic in the same service. The
    // adapter we target has exactly that layout — handle 15 is RESP's
    // CCCD (correct), handle 18 belongs to something further in the
    // service. Until 2026-04-27 the code overwrote on every match and
    // ended up subscribing on handle 18 → adapter never sees notify
    // enable → no responses come back, every PID times out at 1 s.
    //
    // Fix: keep the FIRST CCCD we see — descriptors arrive in
    // ascending-handle order, so that's reliably the one immediately
    // following RESP's value handle.
    static const ble_uuid16_t cccd_uuid = BLE_UUID16_INIT(0x2902);
    if (ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u) == 0
        && s_resp_cccd_handle == 0) {
        s_resp_cccd_handle = dsc->handle;
        ESP_LOGI(TAG, "Found RESP CCCD handle: %d", s_resp_cccd_handle);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GATT characteristic discovery callback
// ---------------------------------------------------------------------------
static int gatt_chr_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        // All characteristics discovered
        if (s_cmd_val_handle == 0 || s_resp_val_handle == 0) {
            ESP_LOGE(TAG, "CMD or RESP characteristic not found");
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
            return 0;
        }

        ESP_LOGI(TAG, "CMD handle=%d, RESP handle=%d",
                 s_cmd_val_handle, s_resp_val_handle);

        // Discover descriptors on RESP characteristic to find its CCCD.
        // This is more robust than assuming CCCD is at val_handle+1.
        int rc = ble_gattc_disc_all_dscs(conn_handle,
                                         s_resp_val_handle,
                                         s_svc_end,
                                         gatt_dsc_disc_cb, nullptr);
        if (rc != 0) {
            ESP_LOGE(TAG, "Descriptor discovery failed: rc=%d", rc);
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Chr disc error: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return 0;
    }

    // Match CMD UUID
    if (ble_uuid_cmp(&chr->uuid.u, &CMD_UUID.u) == 0) {
        s_cmd_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found CMD characteristic, val_handle=%d", s_cmd_val_handle);
    }
    // Match RESP UUID
    if (ble_uuid_cmp(&chr->uuid.u, &RESP_UUID.u) == 0) {
        s_resp_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found RESP characteristic, val_handle=%d", s_resp_val_handle);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GATT service discovery callback
// ---------------------------------------------------------------------------
static int gatt_svc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        if (!s_svc_found) {
            ESP_LOGE(TAG, "Service 0xFFE0 not found");
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
            return 0;
        }
        // Discover characteristics within the service handle range
        int rc = ble_gattc_disc_all_chrs(conn_handle,
                                         s_svc_start, s_svc_end,
                                         gatt_chr_disc_cb, nullptr);
        if (rc != 0) {
            ESP_LOGE(TAG, "Chr discovery start failed: rc=%d", rc);
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Svc disc error: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return 0;
    }

    // Check if this is our target service (0xFFE0)
    if (ble_uuid_cmp(&svc->uuid.u, &SERVICE_UUID.u) == 0) {
        s_svc_found = true;
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
        ESP_LOGI(TAG, "Found service FFE0: handles %d-%d", s_svc_start, s_svc_end);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GAP event callback -- handles scan, connect, disconnect, notify
// ---------------------------------------------------------------------------
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

        case BLE_GAP_EVENT_DISC: {
            // Scan result -- check device name in advertising data
            struct ble_hs_adv_fields fields;
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                             event->disc.length_data);
            if (rc != 0) break;

            if (fields.name != nullptr && fields.name_len > 0) {
                if (fields.name_len == strlen(TARGET_NAME) &&
                    memcmp(fields.name, TARGET_NAME, fields.name_len) == 0) {

                    ESP_LOGI(TAG, "Target found!");
                    s_peer_addr = event->disc.addr;
                    s_state = OBD_FOUND;
                    ble_gap_disc_cancel();
                }
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            // Scan window expired without finding the target
            if (s_state == OBD_SCANNING) {
                s_state    = OBD_IDLE;
                s_retry_ts = millis();
            }
            break;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected, conn_handle=%d",
                         event->connect.conn_handle);
                s_conn_handle = event->connect.conn_handle;

                // Reset discovery state
                s_svc_found = false;
                s_cmd_val_handle  = 0;
                s_resp_val_handle = 0;
                s_resp_cccd_handle = 0;

                // Start service discovery
                int rc = ble_gattc_disc_all_svcs(s_conn_handle,
                                                 gatt_svc_disc_cb, nullptr);
                if (rc != 0) {
                    ESP_LOGE(TAG, "Service discovery failed to start: rc=%d", rc);
                    do_disconnect();
                    s_state    = OBD_ERROR;
                    s_retry_ts = millis();
                }
            } else {
                ESP_LOGW(TAG, "Connect failed: status=%d", event->connect.status);
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_state    = OBD_ERROR;
                s_retry_ts = millis();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "Disconnected: reason=%d", event->disconnect.reason);
            s_conn_handle      = BLE_HS_CONN_HANDLE_NONE;
            s_cmd_val_handle   = 0;
            s_resp_val_handle  = 0;
            s_resp_cccd_handle = 0;
            s_svc_found = false;
            g_state.obd.connected = false;
            g_state.obd_connected = false;
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
            break;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            // Notification received from RESP characteristic
            if (event->notify_rx.attr_handle == s_resp_val_handle) {
                struct os_mbuf *om = event->notify_rx.om;
                uint16_t len = OS_MBUF_PKTLEN(om);
                if (len > sizeof(s_rx_buf)) len = sizeof(s_rx_buf);
                os_mbuf_copydata(om, 0, len, s_rx_buf);
                s_rx_len   = (uint8_t)len;
                s_rx_ready = true;
            }
            break;
        }

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated: conn_handle=%d mtu=%d",
                     event->mtu.conn_handle, event->mtu.value);
            break;

        default:
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NimBLE host task (runs in its own FreeRTOS task)
// ---------------------------------------------------------------------------
static void nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();           // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Called when the NimBLE host and controller are synced
// ---------------------------------------------------------------------------
static void on_sync(void)
{
    // Ensure we have a valid identity address
    ble_hs_util_ensure_addr(0);
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: rc=%d", rc);
    }
    s_ble_synced = true;
    ESP_LOGI(TAG, "BLE host synced -- ready to scan");
}

// ---------------------------------------------------------------------------
// Called on NimBLE host reset
// ---------------------------------------------------------------------------
static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
    s_ble_synced = false;
    // Track resets and bail into IDLE-with-cooldown if they're storming.
    // Without this guard NimBLE retries forever and eventually crashes
    // Core 0 with a load-access fault when the controller goes invalid.
    note_ble_reset(millis());
    // Drop any in-progress connection state so the next sync starts clean.
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    if (s_state != OBD_IDLE) {
        s_state    = OBD_IDLE;
        s_retry_ts = millis();
    }
}

// ---------------------------------------------------------------------------
// Start BLE scan
// ---------------------------------------------------------------------------
static void start_scan(void)
{
    if (!s_ble_synced) return;

    struct ble_gap_disc_params disc_params = {};
    disc_params.filter_duplicates = 1;
    disc_params.passive           = 0;   // active scan
    // Low duty-cycle (10%): reduces BLE advertising-report processing and
    // the resulting heap churn that fragments DRAM.  With high duty-cycle
    // the DRAM heap becomes so fragmented that WiFi AP authentication timers
    // can no longer be allocated.
    disc_params.itvl              = BLE_GAP_SCAN_ITVL_MS(450);
    disc_params.window            = BLE_GAP_SCAN_WIN_MS(45);
    disc_params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;
    disc_params.limited           = 0;

    int rc = ble_gap_disc(s_own_addr_type, SCAN_DURATION_MS,
                          &disc_params, gap_event_cb, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed: rc=%d", rc);
        s_state    = OBD_IDLE;
        s_retry_ts = millis();
        return;
    }
    s_state = OBD_SCANNING;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void obd_bt_init(void)
{
    // Initialize NimBLE port
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Set device name for GAP
    ble_svc_gap_device_name_set("BRL-Laptimer");

    // Configure the NimBLE host
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // NimBLE store config — not needed for unpaired connections
    // ble_store_config_init();

    // Start the NimBLE host task on its own FreeRTOS task
    nimble_port_freertos_init(nimble_host_task);

    s_state    = OBD_IDLE;
    s_retry_ts = 0;
    ESP_LOGI(TAG, "NimBLE init done");
}

void obd_bt_pause(bool paused)
{
    if (paused == s_paused) return;
    s_paused = paused;
    if (paused) {
        // Cancel an in-flight discovery scan if any. Returns BLE_HS_EALREADY
        // when nothing is scanning, which is fine — we ignore the rc.
        ble_gap_disc_cancel();
        if (s_state == OBD_SCANNING || s_state == OBD_FOUND) {
            s_state    = OBD_IDLE;
            s_retry_ts = millis();
        }
        ESP_LOGI(TAG, "Paused (WiFi op in progress)");
    } else {
        ESP_LOGI(TAG, "Resumed");
        s_retry_ts = millis();   // start a new RETRY_INTERVAL countdown
    }
}

// Called from BLE host reset paths to bookkeep + back off if the slave
// is melting down. Returns true when the storm threshold is hit (caller
// should NOT immediately retry).
static bool note_ble_reset(uint32_t now)
{
    s_reset_log[s_reset_idx % RESET_STORM_LIMIT] = now;
    s_reset_idx++;
    if (s_reset_idx >= RESET_STORM_LIMIT) {
        uint32_t oldest = s_reset_log[s_reset_idx % RESET_STORM_LIMIT];
        if (now - oldest < RESET_STORM_WINDOW_MS) {
            s_storm_until = now + RESET_STORM_COOLDOWN;
            ESP_LOGW(TAG, "BLE reset storm — cooldown for %d ms",
                     RESET_STORM_COOLDOWN);
            return true;
        }
    }
    return false;
}

void obd_bt_poll(void)
{
    const uint32_t now = millis();

    // Boot grace period — hold off the first BLE scan for 8 s after
    // boot. The first menu interactions (scrolling, typing) compete
    // with WiFi-AP init traffic on the same SDIO bridge to the C6;
    // an active 5 s BLE-discovery burst on top of that produces
    // visible UI stutter. After the grace period normal scan resumes.
    // 8 s is enough for AP+DHCP+HTTP server to settle and for the
    // user to land on the main menu.
    if (now < 8000) {
        return;
    }

    // Reset-storm cooldown — stay in IDLE until the cooldown ends.
    if (s_storm_until && (int32_t)(now - s_storm_until) < 0) {
        if (s_state != OBD_IDLE) {
            do_disconnect();
            s_state = OBD_IDLE;
        }
        return;
    }
    if (s_storm_until && (int32_t)(now - s_storm_until) >= 0) {
        s_storm_until = 0;
        s_reset_idx   = 0;
    }

    // Periodically give dead PIDs another shot — ECU might have woken up
    // (e.g. AC turned on so its module replies, engine warm so cat-temp
    // PIDs become valid).
    maybe_revive_dead_pids(now);

    switch (s_state) {

        case OBD_IDLE:
            if (!s_ble_synced) break;  // wait for host sync
            if (s_paused) break;       // WiFi has the slave busy
            if (now - s_retry_ts >= RETRY_INTERVAL) {
                start_scan();
            }
            break;

        case OBD_SCANNING:
            // gap_event_cb fires BLE_GAP_EVENT_DISC -> sets s_state = OBD_FOUND
            break;

        case OBD_FOUND: {
            s_state = OBD_CONNECTING;
            ESP_LOGI(TAG, "Connecting...");

            int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr,
                                     5000,     // connect timeout ms
                                     nullptr,  // default connection params
                                     gap_event_cb, nullptr);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_gap_connect failed: rc=%d", rc);
                s_state    = OBD_ERROR;
                s_retry_ts = now;
            }
            break;
        }

        case OBD_CONNECTING:
            // Waiting for BLE_GAP_EVENT_CONNECT callback
            break;

        case OBD_FINGERPRINTING: {
            // Adapter returns the VIN as `[CMD_READ_VIN][STATUS][17 chars]`.
            // Use the last 8 of the VIN (= sequential serial digits) as
            // the cache key — those are the part that's actually unique
            // between two cars of the same model. Fits within NVS's
            // 15-char key limit and is human-readable in logs.
            char key[16] = {};
            bool got_vin = false;
            if (s_rx_ready) {
                if (s_rx_len >= 19 && s_rx_buf[0] == CMD_READ_VIN
                    && s_rx_buf[1] == STATUS_OK) {
                    char vin[18] = {};
                    memcpy(vin, s_rx_buf + 2, 17);
                    // Skip leading nulls/spaces, sanitize to alphanumeric
                    // for safe use as NVS key
                    snprintf(key, sizeof(key), "v_%c%c%c%c%c%c%c%c",
                             vin[9], vin[10], vin[11], vin[12],
                             vin[13], vin[14], vin[15], vin[16]);
                    // Sanitize ONLY the chars snprintf actually wrote.
                    // Iterating up to sizeof(key) overwrites the NUL
                    // terminator and walks into uninitialized memory,
                    // making the log line print past the buffer end.
                    size_t klen = strlen(key);
                    for (size_t i = 2; i < klen; i++) {
                        char c = key[i];
                        if (!(c >= 'A' && c <= 'Z')
                            && !(c >= '0' && c <= '9')
                            && !(c >= 'a' && c <= 'z'))
                            key[i] = '_';
                    }
                    got_vin = true;
                    ESP_LOGI(TAG, "VIN: %.17s → cache key: %s", vin, key);
                } else {
                    ESP_LOGW(TAG, "VIN read declined (status=0x%02X) — "
                             "falling back to car-profile key",
                             s_rx_buf[1]);
                }
                s_rx_ready = false;
            } else if (now - s_req_ts > 3000) {
                // VIN read can be slow on some ECUs (multi-frame ISO-TP).
                // 3s window before falling back.
                ESP_LOGW(TAG, "VIN read timeout — falling back to "
                         "car-profile key");
            } else {
                break;  // still waiting
            }
            if (!got_vin) {
                car_profile_get_active(key, (int)sizeof(key));
                if (key[0] == '\0') {
                    strncpy(key, "default", sizeof(key) - 1);
                }
            }
            rebuild_pid_list(key);
            s_state = OBD_CONNECTED;
            ESP_LOGI(TAG, "Connected to BRL OBD Adapter (cache key: %s)", key);
            for (int i = 0; i < s_pid_count; i++) {
                if (!s_pids[i].dead) { s_pid_idx = i; break; }
            }
            break;
        }

        case OBD_CONNECTED:
            if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                ESP_LOGW(TAG, "Connection lost in CONNECTED state");
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            // Bundle the next 6 alive PIDs into one round-trip. With a
            // typical ~80 ms BLE turnaround, this serves a full HUD page
            // in a single request — Torque-Pro-class throughput. When
            // every PID is dead, gather returns 0 and we idle until the
            // next revive sweep flips one back alive.
            if (gather_inflight() > 0) {
                send_multi_pid_request(s_inflight_pids, s_inflight_count);
            }
            break;

        case OBD_REQUESTING:
            if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            if (s_rx_ready) {
                // Response shape (multi-PID, what we always send now):
                //   [CMD=0x05] [STATUS] [SVC=0x41] [PID1] [d..] [PID2] [d..]...
                // Per-PID payload length comes from the active sensor
                // (OBD.brl knows for sure) or the OBD2 standard table
                // as a fallback.
                bool seen[MULTI_PID_MAX] = {};
                bool ok = false;
                if (s_rx_len >= 4
                    && s_rx_buf[0] == CMD_READ_MULTI_PID
                    && s_rx_buf[1] == STATUS_OK
                    && s_rx_buf[2] == 0x41) {
                    ok = true;
                    const uint8_t *p   = s_rx_buf + 3;
                    const uint8_t *end = s_rx_buf + s_rx_len;
                    while (p < end) {
                        uint8_t pid = *p++;
                        ActivePid *ap = find_active_pid(pid);
                        uint8_t plen = (ap && ap->sensor && ap->sensor->len > 0)
                                       ? (uint8_t)ap->sensor->len
                                       : obd2_standard_pid_len(pid);
                        if (p + plen > end) break;  // truncated tail
                        if (ap) {
                            bool routed = apply_pid(ap->pid, ap->sensor,
                                                    p, plen);
                            ap->strikes = 0;
                            if (ap->dead) {
                                ap->dead = false;
                                ESP_LOGI(TAG, "PID 0x%02X is alive again",
                                         (unsigned)ap->pid);
                            }
                            s_session_had_alive = true;
                            obd_pid_cache_set(ap->pid, PID_ALIVE);
                            if (!routed && !ap->unmapped_logged) {
                                ap->unmapped_logged = true;
                                const char *nm = ap->sensor
                                                 ? ap->sensor->name : "?";
                                ESP_LOGW(TAG,
                                    "PID 0x%02X decoded as '%s' but no "
                                    "dashboard slot routes that name "
                                    "(rename in OBD.brl, or add alias in "
                                    "route_sensor)",
                                    (unsigned)ap->pid, nm);
                            }
                            for (uint8_t i = 0; i < s_inflight_count; i++) {
                                if (s_inflight_pids[i] == pid) {
                                    seen[i] = true;
                                    break;
                                }
                            }
                        }
                        p += plen;
                    }
                }
                // Strike everything we asked for that didn't come back.
                // On a complete failure (status != OK / malformed / no SVC=0x41)
                // `ok` stays false and every inflight PID strikes.
                for (uint8_t i = 0; i < s_inflight_count; i++) {
                    if (ok && seen[i]) continue;
                    ActivePid *ap = find_active_pid(s_inflight_pids[i]);
                    if (!ap) continue;
                    ap->strikes++;
                    if (ap->strikes >= DEAD_THRESHOLD && !ap->dead) {
                        ap->dead = true;
                        // Only persist DEAD if the session has proven the
                        // link works at least once. Otherwise this could
                        // be a transient bus problem, not a real "PID not
                        // supported" — see s_session_had_alive comment.
                        if (s_session_had_alive) {
                            obd_pid_cache_set(ap->pid, PID_DEAD);
                            ESP_LOGI(TAG,
                                "PID 0x%02X marked dead after %d× no-answer "
                                "(persisted to cache)",
                                (unsigned)ap->pid, DEAD_THRESHOLD);
                        } else {
                            ESP_LOGI(TAG,
                                "PID 0x%02X dead in-RAM (cache write held — "
                                "no alive PID seen yet this session)",
                                (unsigned)ap->pid);
                        }
                    }
                }
                s_inflight_count = 0;
                s_state = OBD_CONNECTED;
            } else if (now - s_req_ts > REQ_TIMEOUT_MS) {
                // No notification at all. This is almost always a link-side
                // hiccup (BLE retransmit, adapter busy, ECU briefly slow to
                // assemble the response) — NOT proof that the PIDs in the
                // bundle are unsupported. Striking 4 valid PIDs every time
                // a single request hangs would mass-murder the round-robin.
                // Just drop the request and try again next tick.
                ESP_LOGW(TAG, "Multi-PID request timeout — retrying without "
                              "marking PIDs dead");
                s_inflight_count = 0;
                s_state = OBD_CONNECTED;
            }
            break;

        case OBD_ERROR:
            // do_disconnect() was already called when entering ERROR state
            if (now - s_retry_ts >= RETRY_INTERVAL) {
                s_state    = OBD_IDLE;
                s_retry_ts = now;
            }
            break;
    }
}

OBdBtState obd_bt_state(void) { return s_state; }

void obd_bt_disconnect(void)
{
    do_disconnect();
    s_state    = OBD_IDLE;
    s_retry_ts = 0;
}

// Opaque getter for the cached /cars/OBD.brl profile (see obd_bt.h).
// Returns NULL until OBD.brl has been parsed at least once.
extern "C" const void *obd_bt_pid_profile(void)
{
    return s_obd_profile_fallback.loaded ? &s_obd_profile_fallback : nullptr;
}
