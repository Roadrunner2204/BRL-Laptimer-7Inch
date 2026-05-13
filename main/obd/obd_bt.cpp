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
#include "universal_obd_pids.h"
#include "computed_pids.h"
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
// Mode-01 multi-PID needs the full second: BMW DDE bundles up to 4 PIDs and
// the adapter retransmits on flaky BLE links. A flapping link can take ~700 ms
// to deliver. Single-DID Mode-22 is a different beast — when it works it's
// back in <100 ms, when the ECU doesn't support a DID it's silence forever.
// Waiting 1 s per silent DID across 26 unsupported DIDs starves the alive
// Mode-01 PIDs of polling cycles. 300 ms catches every real response with
// a 3× safety margin and lets dead DIDs get strucken out fast.
// [Source: empirical 04-30 — 26 DIDs × 1 s each = 26 s round-robin starvation]
#define REQ_TIMEOUT_MS   1000   // Mode-01 multi: long, BLE retransmit window
#define DID_TIMEOUT_MS    300   // Mode-22 single: short, ECU answers fast or not at all
// Mode-01 multi-PID timeouts must NOT strike — a flaky BLE retransmit would
// mass-murder 4 valid PIDs at once. Mode-22 single-DID timeouts are safe to
// strike: only one DID per request, so we strike exactly the silent one.
// 3 misses is plenty for Mode-22 since it's deterministic per-DID — either
// the ECU has the DID or it doesn't, no sporadic-response pattern.
#define DID_DEAD_THRESHOLD 3

// ---------------------------------------------------------------------------
// Binary protocol enums
// ---------------------------------------------------------------------------
enum OBDCmd : uint8_t {
    CMD_READ_PID       = 0x01,   // [PID]                  → [SVC, PID, data...]
    CMD_READ_VIN       = 0x02,   // []                     → [VIN 17 chars]
    CMD_READ_MULTI_PID = 0x05,   // [PID1..PID6, max 6]    → [SVC, PID1, d.., PID2, d..]
    CMD_DISCOVER_PIDS  = 0x06,   // []                     → [28 byte bitmap PIDs 0x01..0xE0]
    CMD_READ_DID_22    = 0x10,   // [DID_hi, DID_lo]       → [SVC=0x62, DID, data...]
    // BMW F-Series Extended-Addressing (DDE/DME, EGS, DSC). Adapter sendet
    // an 0x6F1 mit Target-Byte vor PCI, empfängt auf 0x600+TARGET, skipt
    // Source-Byte (0xF1). Antwort-Format auf BLE: identisch zu CMD 0x10 —
    // [CMD][STATUS][SVC=0x62][DID_hi][DID_lo][data...]. TARGET=0x12 für
    // DDE/DME, 0x18 für EGS, 0x29 für DSC.
    CMD_READ_DID_BMW   = 0x16,   // [TARGET, DID_hi, DID_lo] → [SVC=0x62, DID, data...]
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

// Active sensor entry. Each one represents a single OBD2 PID (Mode 01) OR
// a UDS DID (Mode 22) drawn from /cars/OBD.brl + the active vehicle .brl.
// `sensor` always points back to the CarSensor so the decoder uses the
// .brl's scale/offset/start/len.
//
// Dead-PID tracking: when an ECU answers STATUS_TIMEOUT or NEGATIVE
// repeatedly for the same PID, it doesn't support it — keep polling
// it would just waste round-trip time. After DEAD_THRESHOLD strikes
// the PID is skipped in the round-robin. Every REVIVE_INTERVAL_MS we
// flip every dead PID back alive once for a re-test.
//
// Per-sensor live value cache: each successful decode stores `last_value`
// + `last_seen_ms` here so the dashboard slot renderer can pull the
// value back via obd_bt_get_sensor_value(sensor, ...) regardless of
// whether the sensor is a Mode-01 PID or a Mode-22 DID. Replaces the
// global obd_dynamic[256] PID-byte-keyed cache for dynamic slots
// (the global cache stays around for the legacy hardcoded FIELD_RPM
// etc. resolvers in dash_config.cpp).
//
// `unmapped_logged` suppresses the "no dashboard slot routes that name"
// warning after the first hit per sensor — otherwise it spams every cycle.
struct ActivePid {
    uint8_t          proto;           // 7 = OBD2 Mode 01 PID
                                       // 2 = BMW UDS Mode 22 DID
                                       // (= CarSensor.proto)
    uint8_t          pid;             // Mode 01: PID byte (low 8b of can_id)
    uint16_t         did;             // Mode 22: BMW DID (top 16b of can_id)
    uint8_t          target;          // Mode 22 only: BMW ECU target byte
                                       // (low 8b of can_id, default 0x12=DDE)
    const CarSensor *sensor;
    uint8_t          strikes;          // consecutive bad responses
    bool             dead;
    bool             unmapped_logged;  // route_sensor warning fired once

    // Per-sensor live cache
    float            last_value;
    uint32_t         last_seen_ms;     // millis() of last successful decode
    bool             has_value;        // true once a value has been seen
};
#define MAX_ACTIVE_PIDS    64          // up to 32 OBD2 + 32 UDS-DID sensors
// BMW DDE / MSD80 routinely drop 1-2 PIDs out of a 4-PID multi-bundle even
// when those PIDs are perfectly supported. With DEAD_THRESHOLD=3 the pool
// emptied in ~3 s of polling and live data froze. 8 strikes gives sporadic
// PIDs enough headroom to ride out a cluster of misses; revive every 5 s
// so an in-RAM dead PID never stays out of rotation longer than that.
// [Source: empirical 04-30 — log analysis of N47 freeze symptom]
#define DEAD_THRESHOLD     8
#define REVIVE_INTERVAL_MS 5000
// Adapter firmware accepts up to 6 PIDs per request, but BMW E-series ECUs
// (DDE/MSD80) frequently drop one or two answers when 6 are bundled — we
// then strike valid PIDs as missing and they end up dead even though they
// work fine. 4 PIDs per request is the sweet spot that BMW reliably
// returns; only ~50 ms slower than 6 per round-trip.
#define MULTI_PID_MAX      6   // Adapter erlaubt 6, Antwort wird ISO-TP-Multi-Frame

// CarSensor.proto values we care about (CAN-Checked .brl convention):
#define BRL_PROTO_PT_CAN     0   // 11-bit broadcast (only via CAN-Direct)
#define BRL_PROTO_UDS_BMW    2   // BMW DDE/DKG UDS Mode 22 — our new path
#define BRL_PROTO_OBD2_MODE1 7   // standard OBD2 Mode 01 (existing path)
static ActivePid s_pids[MAX_ACTIVE_PIDS];
static uint8_t   s_pid_count   = 0;
static uint8_t   s_pid_idx     = 0;
static uint32_t  s_last_revive_ms = 0;

// In-flight request bookkeeping. Two flavours:
//   proto=7 (OBD2 Mode 01): up to MULTI_PID_MAX PIDs in s_inflight_pids[]
//   proto=2 (BMW UDS 22):   exactly 1 DID in s_inflight_did
// `s_inflight_idx[]` records the s_pids[] index for each inflight entry so
// the response handler can attribute strikes / store last_value back to
// the correct ActivePid even when two .brls map the same PID twice.
static uint8_t   s_inflight_pids[MULTI_PID_MAX] = {};
static uint8_t   s_inflight_idx[MULTI_PID_MAX]  = {};
static uint8_t   s_inflight_count               = 0;
static uint8_t   s_inflight_proto               = 0;   // 7 or 2; 0 when idle
static uint16_t  s_inflight_did                 = 0;   // for proto=2
static uint8_t   s_inflight_target              = 0;   // for proto=2 BMW

// One-shot diagnostics flags for BMW Mode-22. Reset on disconnect so each
// new connect produces fresh "first request / first success" logs.
static bool      s_bmw_first_req_logged          = false;
static bool      s_bmw_first_ok_logged           = false;

// Cache-poisoning guard. We persist a PID as DEAD only after the session
// has seen at least one *real* response come back. Without this, a stuck
// CAN bus or a transient adapter problem during the first 3 round-trips
// could write every PID as dead into NVS — and the next connect would
// then start with 0 alive PIDs, even though the car is healthy. Once a
// single STATUS_OK lands, we know the link is sane and DEAD writes are
// trustworthy.
static bool      s_session_had_alive             = false;

// Universal-OBD2-Liste — fest einkompiliert (universal_obd_pids.h), eine
// einzige Quelle der Wahrheit für OBD-BLE. Wird beim ersten rebuild_pid_list
// einmal in s_obd_profile_fallback kopiert und dann immer wiederverwendet.
// Vehicle-spezifische Profile (BMW UDS, motor-specific Mode-01 Extras) leben
// daneben in s_vehicle_profile.
static CarProfile s_obd_profile_fallback = {};
static bool       s_obd_profile_tried    = false;

static int count_obd2_sensors(const CarProfile *p)
{
    if (!p || !p->loaded) return 0;
    int n = 0;
    for (int i = 0; i < p->sensor_count; i++) {
        if (p->sensors[i].proto == BRL_PROTO_OBD2_MODE1) n++;
    }
    return n;
}

// Append every sensor of the given proto from `p` into s_pids[]. The
// .brl `can_id` encoding for these two protos:
//   proto=7 (OBD2 Mode 01)   →  can_id = PID byte (low 8 bits used)
//   proto=2 (BMW UDS Mode 22) →  can_id = (DID << 8) | bank, where
//                                 DID = top 16 bits, bank = 0x01 (DDE)
// PT-CAN broadcast (proto=0) is intentionally skipped — that lives on
// the passive bus side and is only reachable when the user runs the
// laptimer in CAN-Direct mode (on-board TJA1051 hardwired to PT-CAN).
static void append_sensors_of_proto(const CarProfile *p, uint8_t proto)
{
    if (!p || !p->loaded) return;
    for (int i = 0; i < p->sensor_count; i++) {
        const CarSensor *s = &p->sensors[i];
        if (s->proto != proto) continue;
        if (s_pid_count >= MAX_ACTIVE_PIDS) break;

        ActivePid &ap = s_pids[s_pid_count];
        ap.proto           = proto;
        ap.sensor          = s;
        ap.strikes         = 0;
        ap.dead            = false;
        ap.unmapped_logged = false;
        ap.has_value       = false;
        ap.last_value      = 0.0f;
        ap.last_seen_ms    = 0;

        if (proto == BRL_PROTO_OBD2_MODE1) {
            ap.pid    = (uint8_t)(s->can_id & 0xFFu);
            ap.did    = 0;
            ap.target = 0;
            // Discovery-Filter: wenn die Bitmap vom Adapter sagt dass das
            // Auto diesen PID nicht unterstützt, gleich tot markieren — wir
            // verschwenden keine Polling-Round-Trips auf 8 Strikes Lernphase.
            // Wenn keine Discovery vorliegt (Adapter v1.0 oder Discovery-
            // Antwort timeout-te), liefert obd_pid_is_supported immer true →
            // Verhalten unverändert, nur try-and-cache.
            if (!obd_pid_is_supported(ap.pid)) {
                ap.dead    = true;
                ap.strikes = DEAD_THRESHOLD;
                ESP_LOGI(TAG,
                    "  PID 0x%02X '%s'  → SKIP (nicht in Discovery-Bitmap)",
                    (unsigned)ap.pid, s->name);
            } else {
                ESP_LOGI(TAG,
                    "  PID 0x%02X '%s'  start=%d len=%d scale=%.6f "
                    "offset=%.3f  unsigned=%d",
                    (unsigned)ap.pid, s->name,
                    (int)s->start, (int)s->len,
                    (double)s->scale, (double)s->offset,
                    (int)s->is_unsigned);
            }
        } else if (proto == BRL_PROTO_UDS_BMW) {
            // BMW DID is the top 16 bits of can_id, target byte is low 8 bits.
            //
            // 2026-05-04: Bug-Fix. CANchecked Original-TRX (N47F.TRX) hat
            // für ALLE DDE-DIDs target=0x01 — das ist die korrekte Adresse
            // für den N47-DDE auf F-Series. Die alte Heuristik
            // "0x00/0x01 = legacy → ersetze durch 0x12" hat das auf
            // 0x12 (E-Series-DDE) überschrieben und damit die DDE komplett
            // unhörbar gemacht. Im F20-Test 2026-05-03 kamen 0/180 Antworten.
            //
            // Jetzt: target = bank byte, 1:1 wie in CANchecked. Nur 0x00
            // (echtes "leer") fällt auf 0x12 als E-Series-Default zurück.
            ap.pid    = 0;
            ap.did    = (uint16_t)((s->can_id >> 8) & 0xFFFFu);
            uint8_t bank = (uint8_t)(s->can_id & 0xFFu);
            ap.target = (bank == 0x00) ? 0x12 : bank;
            ESP_LOGI(TAG,
                "  DID 0x%04X '%s'  target=0x%02X  start=%d len=%d "
                "scale=%.6f offset=%.3f  unsigned=%d  (UDS Mode 22)",
                (unsigned)ap.did, s->name, (unsigned)ap.target,
                (int)s->start, (int)s->len,
                (double)s->scale, (double)s->offset,
                (int)s->is_unsigned);
        }
        s_pid_count++;
    }
}

// Build the active sensor list. The BLE adapter handles two protocols:
//   - OBD2 Mode 01 (proto=7) — universal, decoder uses CMD_READ_PID /
//                              CMD_READ_MULTI_PID, sourced from the
//                              einkompilierten Universal-OBD-Liste
//                              (universal_obd_pids.h)
//   - UDS BMW Mode 22 (proto=2) — vehicle-specific, decoder uses
//                                  CMD_READ_DID_22, sourced from the
//                                  active vehicle profile (e.g.
//                                  /cars/N47F.brl) selected by the
//                                  user in Settings → Vehicle.
//
// PT-CAN broadcast (proto=0) sensors are intentionally not selectable
// over the adapter — the adapter has no passive-listen command. Those
// only become visible when the user runs the laptimer in CAN-Direct
// mode (on-board TJA1051 hardwired to PT-CAN).
//
// Order:
//   1. Universal-OBD2-Liste (einkompiliert)         — generic OBD2 baseline
//   2. /cars/<active>.brl proto=7 (extra OBD2-PIDs) — vehicle-specific
//   3. /cars/<active>.brl proto=2 (UDS DIDs)        — vehicle-specific
//   4. fallback DEFAULT_PIDS if nothing else loaded
//
// `cache_key` identifies the connected vehicle for the PID cache.
static CarProfile s_vehicle_profile = {};   // active /cars/<NAME>.brl
static bool       s_vehicle_tried   = false;
static char       s_vehicle_name[32] = {};

static void rebuild_pid_list(const char *cache_key)
{
    s_pid_count = 0;
    s_pid_idx   = 0;

    obd_pid_cache_load(cache_key);

    // 1. Universal-OBD-Liste fest einkompiliert (universal_obd_pids.h).
    //    Eine einzige Quelle der Wahrheit — kein SD-Override, kein Server-
    //    Download. OBD-BLE-Mode ist generisch genug dass alle Autos vom
    //    selben PID-Pool bedient werden; die Discovery-Bitmap filtert
    //    runter auf das was das jeweilige Auto wirklich antwortet.
    //    Vehicle-spezifische Sensoren (BMW UDS DIDs etc.) leben weiter
    //    im aktiven Vehicle-Profile (N47F.brl etc., siehe Schritt 2+3).
    if (!s_obd_profile_tried) {
        s_obd_profile_tried = true;
        memset(&s_obd_profile_fallback, 0, sizeof(s_obd_profile_fallback));
        strncpy(s_obd_profile_fallback.name,   "Universal OBD2",
                sizeof(s_obd_profile_fallback.name)   - 1);
        strncpy(s_obd_profile_fallback.engine, "Generic",
                sizeof(s_obd_profile_fallback.engine) - 1);
        int n = UNIVERSAL_OBD_SENSOR_COUNT;
        if (n > CAR_MAX_SENSORS) n = CAR_MAX_SENSORS;
        for (int i = 0; i < n; i++) {
            s_obd_profile_fallback.sensors[i] = UNIVERSAL_OBD_SENSORS[i];
        }
        s_obd_profile_fallback.sensor_count = n;
        s_obd_profile_fallback.loaded       = true;
        ESP_LOGI(TAG, "Universal-OBD-Liste eingebaut geladen: %d Sensoren", n);
    }

    // 2+3. Load the user-selected vehicle profile so we can emit its
    //      Mode-22 DIDs (and any extra Mode-01 PIDs not in the Universal-
    //      Liste). Defensive: ignore stale "OBD.brl" entries from old NVS
    //      configs — the Universal-Liste is already loaded above and a
    //      duplicate OBD.brl on SD would just double the dead-cache.
    char active_name[32];
    car_profile_get_active(active_name, sizeof(active_name));
    if (strcasecmp(active_name, "OBD.brl") == 0) {
        active_name[0] = '\0';
    }
    if (active_name[0]
        && (!s_vehicle_tried || strcmp(active_name, s_vehicle_name) != 0)) {
        s_vehicle_tried = true;
        strncpy(s_vehicle_name, active_name, sizeof(s_vehicle_name) - 1);
        s_vehicle_name[sizeof(s_vehicle_name) - 1] = '\0';
        memset(&s_vehicle_profile, 0, sizeof(s_vehicle_profile));
        if (car_profile_load_into(active_name, &s_vehicle_profile)) {
            int n_obd2 = 0, n_uds = 0, n_ptcan = 0;
            for (int i = 0; i < s_vehicle_profile.sensor_count; i++) {
                switch (s_vehicle_profile.sensors[i].proto) {
                    case BRL_PROTO_OBD2_MODE1: n_obd2++;  break;
                    case BRL_PROTO_UDS_BMW:    n_uds++;   break;
                    case BRL_PROTO_PT_CAN:     n_ptcan++; break;
                    default: break;
                }
            }
            ESP_LOGI(TAG, "Loaded vehicle profile %s: %d OBD2, %d UDS, "
                          "%d PT-CAN (PT-CAN only via CAN-Direct mode)",
                     active_name, n_obd2, n_uds, n_ptcan);
        } else {
            ESP_LOGW(TAG, "Vehicle profile %s not found", active_name);
        }
    }

    // Append in priority order. Universal-OBD2 first so generic PIDs always
    // populate; vehicle-specific overrides come second (sensors with
    // duplicate PIDs end up in s_pids[] twice but that's fine — both
    // get polled, dynamic slots can pick whichever they reference by
    // name/index).
    if (count_obd2_sensors(&s_obd_profile_fallback) > 0) {
        append_sensors_of_proto(&s_obd_profile_fallback, BRL_PROTO_OBD2_MODE1);
        ESP_LOGI(TAG, "Universal OBD2: %d Mode-01 PIDs active",
                 s_pid_count);
    }
    if (s_vehicle_profile.loaded) {
        int before = s_pid_count;
        append_sensors_of_proto(&s_vehicle_profile, BRL_PROTO_OBD2_MODE1);
        append_sensors_of_proto(&s_vehicle_profile, BRL_PROTO_UDS_BMW);
        ESP_LOGI(TAG, "Vehicle profile: +%d sensors (Mode-01 + UDS)",
                 s_pid_count - before);
    }
    if (s_pid_count == 0) {
        // Last resort — hardcoded defaults (RPM/TPS/MAP/Coolant/Intake)
        for (uint8_t p : DEFAULT_PIDS) {
            s_pids[s_pid_count].proto         = BRL_PROTO_OBD2_MODE1;
            s_pids[s_pid_count].pid           = p;
            s_pids[s_pid_count].did           = 0;
            s_pids[s_pid_count].sensor        = nullptr;
            s_pids[s_pid_count].strikes       = 0;
            s_pids[s_pid_count].dead          = false;
            s_pids[s_pid_count].unmapped_logged = false;
            s_pids[s_pid_count].has_value     = false;
            s_pids[s_pid_count].last_value    = 0.0f;
            s_pids[s_pid_count].last_seen_ms  = 0;
            s_pid_count++;
        }
        ESP_LOGI(TAG, "Using %d built-in OBD2 PIDs (no .brl on SD)",
                 s_pid_count);
    }

    // Apply the cache as a *hint* rather than gospel. Earlier sessions
    // could have been running with a buggy decoder (off-by-one start
    // offset, wrong scaling) that mis-marked perfectly working PIDs as
    // dead. We mustn't let those past mistakes lock the user out of
    // valid sensors forever.
    //
    // New strategy: cache-dead PIDs start *alive* but with `strikes` set
    // close to DEAD_THRESHOLD — i.e. only a few bad responses away from
    // being dead again. We give them 3 chances rather than 1 because BMW
    // DDE drops PIDs out of multi-bundles intermittently — a single miss
    // is not proof of unsupported. So:
    //   - if the PID really is unsupported, 3 no-answers flip it dead and
    //     we save the round-trip on every subsequent cycle.
    //   - if the PID actually works, the first answer resets strikes to 0
    //     and the cache is corrected on the next save.
    int seeded_hint = 0;
    for (int i = 0; i < s_pid_count; i++) {
        if (obd_pid_cache_is_dead(s_pids[i].pid)) {
            s_pids[i].dead    = false;          // start alive
            s_pids[i].strikes = DEAD_THRESHOLD - 3;  // 3 chances before re-dying
            seeded_hint++;
        }
    }
    if (seeded_hint > 0) {
        ESP_LOGI(TAG, "PID cache: %d previously-dead PIDs on probation "
                      "(3 chances before re-marking dead)",
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
// (apply_profile_sensor was the original, single-proto Mode-01 decoder;
//  it's been merged into apply_profile_sensor_cached below which is
//  proto-aware and ALSO writes the per-sensor live cache. Kept as a
//  comment-only stub for readers tracking the refactor.)

// Decode a sensor response and record the post-scale value in the
// matching ActivePid's per-sensor live cache. `d`/`len` is always the
// raw DATA payload — the caller has stripped the response header
// ([0x41][PID] for Mode 01, [DID_hi][DID_lo] for Mode 22). The .brl
// uses different `start` conventions per proto:
//   proto=7 (Mode 01): start = byte offset in full CAN frame
//                              [PCI][0x41][PID][D1][D2] → minus 3
//                              gives the DATA-buffer offset
//   proto=2 (Mode 22): start = byte offset in DATA already → 0
// We adjust accordingly here so a single decoder serves both paths.
//
// Used by both the Mode 01 and Mode 22 response parsers so dynamic
// dashboard slots (IDs 128+N) read identical-shape values regardless
// of how the underlying CarSensor is sourced.
static bool apply_profile_sensor_cached(int pid_idx,
                                        const uint8_t *d, uint8_t len)
{
    if (pid_idx < 0 || pid_idx >= s_pid_count) return false;
    ActivePid *ap = &s_pids[pid_idx];
    const CarSensor *s = ap->sensor;
    if (!s) return false;

    int32_t off = (int32_t)s->start;
    if (ap->proto == BRL_PROTO_OBD2_MODE1) off -= 3;   // CAN-frame → data
    if (off < 0) off = 0;
    if (off >= len) return false;

    int32_t raw = 0;
    if (s->len == 2 && off + 1 < len) {
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

    // Per-sensor live cache → consumed by dash_config field_format_value
    // for slot IDs ≥ 128 (dynamic dashboard slots).
    ap->last_value    = value;
    ap->last_seen_ms  = millis();
    ap->has_value     = true;

    // Legacy global PID-keyed cache (only valid for proto=7 since the
    // key is one byte). Skip for proto=2 — DIDs are 16-bit and don't
    // collide with PIDs in this 256-entry array.
    if (ap->proto == BRL_PROTO_OBD2_MODE1) {
        obd_dynamic_set(ap->pid, value);
    }
    // Computed-PIDs füttern (Boost-Calc, AFR, Power, Torque). Hinkt 0 ms
    // hinter dem Live-Wert hinterher — wird beim nächsten Render abgefragt.
    computed_update(s->name, value);
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
// ---------------------------------------------------------------------------
// Round-robin helpers — skip dead PIDs so we don't waste adapter round-trips
// on things the ECU never answers, and bundle up to MULTI_PID_MAX live PIDs
// per request so one ~80 ms BLE round-trip serves 6 dashboard slots.
// (The previous static apply_pid() and find_active_pid() helpers are gone —
// the response parsers now go through apply_profile_sensor_cached() with
// per-inflight-slot bookkeeping, which handles both Mode 01 and Mode 22.)
// ---------------------------------------------------------------------------

// Walk forward from s_pid_idx and pack up to MULTI_PID_MAX alive PIDs into
// s_inflight_pids[]. Advances s_pid_idx past the last gathered slot so the
// next call continues round-robin where this one stopped. If every PID is
// dead, returns 0 — caller should idle and let maybe_revive_dead_pids() try.
// One round-robin step. Picks a single proto-flavour for this batch
// (whatever the next alive sensor uses) and gathers as many compatible
// alive sensors as the adapter accepts in one request:
//   proto=7 → up to MULTI_PID_MAX PIDs in one CMD_READ_MULTI_PID
//   proto=2 → exactly 1 DID  in one CMD_READ_DID_22
// Mixing protos in one request isn't possible — adapter has no
// "multi-DID" command and Mode-22 reads one DID at a time. A purely
// proto=2 vehicle profile (no Mode-01 sensors) ends up with one DID
// per ~80 ms tick = ~30 sensors per ~2.5 s round-robin cycle, which
// is fast enough for everything except RPM-style high-rate values
// (RPM is always Mode-01 anyway).
static uint8_t gather_inflight(void)
{
    s_inflight_count  = 0;
    s_inflight_proto  = 0;
    s_inflight_did    = 0;
    s_inflight_target = 0;
    if (s_pid_count == 0) return 0;

    // Find the first alive sensor — its proto locks the batch flavour.
    int first_idx = -1;
    for (int tries = 0; tries < s_pid_count; tries++) {
        int idx = (s_pid_idx + tries) % s_pid_count;
        if (!s_pids[idx].dead) { first_idx = idx; break; }
    }
    if (first_idx < 0) return 0;
    s_inflight_proto = s_pids[first_idx].proto;

    int last_picked = -1;
    int max_count   = (s_inflight_proto == BRL_PROTO_OBD2_MODE1)
                      ? MULTI_PID_MAX : 1;

    for (int tries = 0;
         tries < s_pid_count && s_inflight_count < max_count;
         tries++) {
        int idx = (s_pid_idx + tries) % s_pid_count;
        if (s_pids[idx].dead) continue;
        if (s_pids[idx].proto != s_inflight_proto) continue;

        s_inflight_idx[s_inflight_count] = (uint8_t)idx;
        if (s_inflight_proto == BRL_PROTO_OBD2_MODE1) {
            s_inflight_pids[s_inflight_count] = s_pids[idx].pid;
        } else {
            // proto=2: only one fits per batch. Stash the DID + target
            // separately so the dispatcher can pull them for the BMW
            // Extended-Addressing request.
            s_inflight_did    = s_pids[idx].did;
            s_inflight_target = s_pids[idx].target;
        }
        s_inflight_count++;
        last_picked = idx;
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

// 28-byte Bitmap supporteter Mode-01 PIDs (vom Adapter via CMD_DISCOVER_PIDS).
// Layout MSB-first: byte[0] bit7 = PID 0x01 ... byte[27] bit0 = PID 0xE0.
// s_pid_disc_complete = true sobald die Antwort erfolgreich verarbeitet wurde.
// Vor dem ersten Connect (oder wenn Adapter v1.0 = ohne Discovery): false →
// is_supported() liefert immer true und wir fallen aufs alte Lernverhalten
// zurück (try-and-mark-dead).
static uint8_t s_supported_pids[28] = {0};
static bool    s_pid_disc_complete  = false;

// Brücke zwischen OBD_FINGERPRINTING (VIN-Read fertig) und OBD_DISCOVERING
// (Bitmap-Antwort empfangen): wir brauchen den Cache-Key (von VIN abgeleitet
// oder Profil-Fallback) erst NACH der Discovery wieder, weil rebuild_pid_list
// die Bitmap nutzt um nicht-supportete PIDs sofort dead zu markieren.
static char s_pending_cache_key[16] = {0};

bool obd_pid_discovery_complete(void) { return s_pid_disc_complete; }

bool obd_pid_is_supported(uint8_t pid)
{
    if (!s_pid_disc_complete) return true;   // unbekannt → nicht filtern
    if (pid == 0x00 || pid == 0x20 || pid == 0x40 || pid == 0x60
        || pid == 0x80 || pid == 0xA0 || pid == 0xC0)
        return true;                          // Anchor-PIDs sind per Definition probierbar
    if (pid < 0x01 || pid > 0xE0) return false;
    uint8_t bit_idx  = (uint8_t)(pid - 1);    // PID 0x01 → bit 0
    uint8_t byte_idx = bit_idx / 8;
    uint8_t bit_pos  = 7 - (bit_idx % 8);     // MSB-first innerhalb des Bytes
    return (s_supported_pids[byte_idx] & (1u << bit_pos)) != 0;
}

// Adapter ≥ v1.1 antwortet auf CMD_DISCOVER_PIDS mit der 28-byte Bitmap.
// v1.0 antwortet mit ERR_NO_RESP / ERR_NOT_INIT — wir akzeptieren das und
// laufen ohne Filter weiter (s_pid_disc_complete bleibt false).
static void send_discover_request(void)
{
    if (s_cmd_val_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t cmd[1] = { CMD_DISCOVER_PIDS };
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_cmd_val_handle,
                                         cmd, sizeof(cmd));
    if (rc != 0) {
        ESP_LOGW(TAG, "Write DISCOVER_PIDS failed: rc=%d", rc);
        return;
    }
    s_rx_ready = false;
    s_req_ts   = millis();
}

// Send a UDS Mode 22 ReadDataByIdentifier for the given 16-bit DID.
// Uses CMD_READ_DID_BMW (0x16) with explicit target byte — the adapter
// puts ID 0x6F1 on the wire with [target][PCI=0x03][0x22][DID_hi][DID_lo]
// and listens on 0x600+target. Without the target byte BMW DDE/DME on
// F-Series and late E-Series silently ignores Mode 22 requests on the
// standard OBD2 0x7E0/0x7E8 pair (only generic Mode 01 PIDs proxy
// through the gateway there).
//
// Response shape from the adapter: `[0x16][STATUS][SVC=0x62][DID_hi][DID_lo][data...]`.
// We strip the [DID_hi][DID_lo] echo before passing data to the decoder
// so the .brl `start` field indexes the same byte as on the Mode-01 path.
static void send_did22_request(uint16_t did, uint8_t target)
{
    if (s_cmd_val_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t cmd[4] = {
        CMD_READ_DID_BMW,
        target,
        (uint8_t)((did >> 8) & 0xFFu),
        (uint8_t)(did & 0xFFu),
    };

    // Erste BMW-Anfrage einer Session loggen — bestätigt dass der neue
    // 0x16-Pfad benutzt wird und nicht das alte 0x10. Wenn diese Zeile
    // gar nicht im Log auftaucht aber die DIDs trotzdem stumm bleiben,
    // wird der Mode-22-Pool gar nicht erreicht (z.B. weil .brl keine
    // proto=2-Sensoren hat).
    if (!s_bmw_first_req_logged) {
        s_bmw_first_req_logged = true;
        ESP_LOGI(TAG, "First BMW Mode-22 request: DID 0x%04X target=0x%02X "
                      "(via CMD_READ_DID_BMW=0x16)",
                 (unsigned)did, (unsigned)target);
    }

    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_cmd_val_handle,
                                         cmd, sizeof(cmd));
    if (rc != 0) {
        ESP_LOGW(TAG, "Write DID 0x%04X (target 0x%02X) failed: rc=%d",
                 (unsigned)did, (unsigned)target, rc);
        return;
    }
    s_rx_ready = false;
    s_req_ts   = millis();
    s_state    = OBD_REQUESTING;
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

                // Connection-Interval auf Minimum drücken — wir wollen
                // Torque-Pro-Geschwindigkeit. Default war 30-50ms, das macht
                // jeden BLE-Roundtrip langsam. 7.5-15ms = ~6× schneller.
                //   itvl_min/max in 1.25 ms units → 6 = 7.5 ms, 12 = 15 ms
                //   latency=0 (Slave muss jeden Slot aktiv sein)
                //   supervision_timeout=400 (= 4 s)
                struct ble_gap_upd_params cp = {};
                cp.itvl_min            = 6;
                cp.itvl_max            = 12;
                cp.latency             = 0;
                cp.supervision_timeout = 400;
                cp.min_ce_len          = 0;
                cp.max_ce_len          = 0;
                int up = ble_gap_update_params(s_conn_handle, &cp);
                if (up == 0) {
                    ESP_LOGI(TAG, "Conn-Interval-Update angefragt: 7.5-15ms");
                } else {
                    ESP_LOGW(TAG, "Conn-Interval-Update failed rc=%d "
                             "(läuft mit Default ~30-50ms weiter)", up);
                }

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
            s_bmw_first_req_logged = false;
            s_bmw_first_ok_logged  = false;
            // Discovery-Bitmap verfällt mit der Connection — bei Reconnect
            // ggf. anderes Auto, da darf eine alte Bitmap nicht stehenbleiben.
            s_pid_disc_complete = false;
            memset(s_supported_pids, 0, sizeof(s_supported_pids));
            // Computed-Werte (AFR-Stoich, MAP, Lambda etc.) auch invalidieren
            // — beim nächsten Connect kommt evtl. ein anderes Auto/Fuel-Type.
            computed_reset();
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
            // Cache-Key für später aufheben — rebuild_pid_list passiert erst
            // nach DISCOVERING (sonst weiß der List-Builder nicht welche PIDs
            // zu skippen). s_pending_cache_key ist File-scope (oben deklariert).
            strncpy(s_pending_cache_key, key, sizeof(s_pending_cache_key) - 1);
            s_pending_cache_key[sizeof(s_pending_cache_key) - 1] = '\0';

            // PID-Discovery anstoßen — Adapter pollt die supported-PIDs-Bitmap.
            // Adapter v1.0 (ohne Discovery) antwortet ERR_NO_RESP → wir laufen
            // ohne Filter weiter, das wird in OBD_DISCOVERING abgefangen.
            ESP_LOGI(TAG, "VIN done, querying supported PIDs (DISCOVER_PIDS)");
            s_pid_disc_complete = false;
            memset(s_supported_pids, 0, sizeof(s_supported_pids));
            send_discover_request();
            s_state = OBD_DISCOVERING;
            break;
        }

        case OBD_DISCOVERING: {
            // Adapter-Antwort: [CMD_DISCOVER_PIDS][STATUS][28 byte bitmap]
            // = 30 byte. Bei v1.0-Adapter (ohne Discovery): [CMD][NICHT_OK].
            // Bei Bus-Fehler: ERR_NOT_INIT. Bei 1.5s Timeout: einfach weiter
            // ohne Bitmap — alte try-and-cache Pollerei greift dann wieder.
            bool proceed = false;
            if (s_rx_ready) {
                if (s_rx_len >= 30 && s_rx_buf[0] == CMD_DISCOVER_PIDS
                    && s_rx_buf[1] == STATUS_OK) {
                    memcpy(s_supported_pids, s_rx_buf + 2, 28);
                    s_pid_disc_complete = true;
                    int n_supp = 0;
                    for (int i = 0; i < 28; i++) {
                        for (int b = 0; b < 8; b++) {
                            if (s_supported_pids[i] & (1u << b)) n_supp++;
                        }
                    }
                    ESP_LOGI(TAG, "PID-Discovery: %d supportete PIDs", n_supp);
                } else {
                    ESP_LOGW(TAG, "PID-Discovery deklined (len=%d, status=0x%02X) — "
                             "fallback auf try-and-cache",
                             (int)s_rx_len,
                             s_rx_len >= 2 ? s_rx_buf[1] : 0xFF);
                }
                s_rx_ready = false;
                proceed = true;
            } else if (now - s_req_ts > 1500) {
                ESP_LOGW(TAG, "PID-Discovery timeout — Adapter v1.0? "
                         "fallback auf try-and-cache");
                proceed = true;
            }
            if (!proceed) break;

            rebuild_pid_list(s_pending_cache_key);
            s_state = OBD_CONNECTED;
            ESP_LOGI(TAG, "Connected to BRL OBD Adapter (cache key: %s)",
                     s_pending_cache_key);
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
            // Round-robin step. gather_inflight() picks one proto-flavour
            // (Mode 01 multi or Mode 22 single) based on what's next in
            // the rotation, then we dispatch the appropriate command.
            if (gather_inflight() > 0) {
                if (s_inflight_proto == BRL_PROTO_OBD2_MODE1) {
                    send_multi_pid_request(s_inflight_pids, s_inflight_count);
                } else if (s_inflight_proto == BRL_PROTO_UDS_BMW) {
                    send_did22_request(s_inflight_did, s_inflight_target);
                }
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
                bool seen[MULTI_PID_MAX] = {};
                bool ok = false;

                // Multi-PID Mode 01 response shape:
                //   [CMD=0x05] [STATUS] [SVC=0x41] [PID1] [d..] [PID2] [d..]...
                // Single-DID Mode 22 response shape:
                //   [CMD=0x10] [STATUS] [SVC=0x62] [DID_hi] [DID_lo] [data...]
                if (s_inflight_proto == BRL_PROTO_OBD2_MODE1
                    && s_rx_len >= 4
                    && s_rx_buf[0] == CMD_READ_MULTI_PID
                    && s_rx_buf[1] == STATUS_OK
                    && s_rx_buf[2] == 0x41) {
                    ok = true;
                    const uint8_t *p   = s_rx_buf + 3;
                    const uint8_t *end = s_rx_buf + s_rx_len;
                    while (p < end) {
                        uint8_t pid = *p++;
                        // Find the inflight slot this PID belongs to (so
                        // we credit the right ActivePid even if two .brls
                        // mapped the same PID twice — the inflight index
                        // remembers which one we asked for).
                        int matched = -1;
                        for (uint8_t i = 0; i < s_inflight_count; i++) {
                            if (s_inflight_pids[i] == pid) {
                                matched = s_inflight_idx[i];
                                seen[i] = true;
                                break;
                            }
                        }
                        if (matched < 0) matched = -1;
                        const CarSensor *sensor = (matched >= 0)
                                                  ? s_pids[matched].sensor
                                                  : nullptr;
                        uint8_t plen = (sensor && sensor->len > 0)
                                       ? (uint8_t)sensor->len
                                       : obd2_standard_pid_len(pid);
                        if (p + plen > end) break;
                        if (matched >= 0) {
                            ActivePid &ap = s_pids[matched];
                            // Single decoder pass — proto-aware, writes
                            // per-sensor cache + global obd_dynamic +
                            // route_sensor for the legacy fields.
                            bool routed =
                                apply_profile_sensor_cached(matched, p, plen);
                            ap.strikes = 0;
                            if (ap.dead) {
                                ap.dead = false;
                                ESP_LOGI(TAG, "PID 0x%02X is alive again",
                                         (unsigned)ap.pid);
                            }
                            s_session_had_alive = true;
                            obd_pid_cache_set(ap.pid, PID_ALIVE);
                            if (!routed && !ap.unmapped_logged) {
                                ap.unmapped_logged = true;
                                ESP_LOGW(TAG,
                                    "PID 0x%02X '%s' — no legacy slot but "
                                    "available via dynamic picker.",
                                    (unsigned)ap.pid,
                                    ap.sensor ? ap.sensor->name : "?");
                            }
                        }
                        p += plen;
                    }
                } else if (s_inflight_proto == BRL_PROTO_UDS_BMW
                           && s_rx_len >= 5
                           && (s_rx_buf[0] == CMD_READ_DID_BMW
                               || s_rx_buf[0] == CMD_READ_DID_22)
                           && s_rx_buf[1] == STATUS_OK
                           && s_rx_buf[2] == 0x62) {
                    // Single-DID Mode 22. [CMD][STATUS][0x62][DID_hi][DID_lo][data...]
                    uint16_t resp_did = ((uint16_t)s_rx_buf[3] << 8)
                                       | s_rx_buf[4];
                    if (s_inflight_count == 1
                        && resp_did == s_inflight_did) {
                        ok = true;
                        seen[0] = true;
                        int matched = s_inflight_idx[0];
                        if (matched >= 0 && matched < s_pid_count) {
                            ActivePid &ap = s_pids[matched];
                            // Pass DATA bytes only (after [SVC][DID_hi][DID_lo]
                            // header). apply_profile_sensor_cached uses
                            // proto=2's start-as-data-offset convention.
                            const uint8_t *data = s_rx_buf + 5;
                            uint8_t data_len = (uint8_t)(s_rx_len - 5);
                            apply_profile_sensor_cached(matched,
                                                        data, data_len);
                            ap.strikes = 0;
                            if (ap.dead) {
                                ap.dead = false;
                                ESP_LOGI(TAG, "DID 0x%04X is alive again",
                                         (unsigned)ap.did);
                            }
                            // Erste erfolgreiche BMW-Mode-22-Antwort einer
                            // Session laut loggen — bestätigt End-to-End-
                            // Funktion (Adapter → ECU → Adapter → Laptimer).
                            if (!s_bmw_first_ok_logged) {
                                s_bmw_first_ok_logged = true;
                                ESP_LOGI(TAG,
                                    "First BMW Mode-22 response: DID 0x%04X "
                                    "target=0x%02X '%s' (%u data bytes)",
                                    (unsigned)ap.did, (unsigned)ap.target,
                                    ap.sensor ? ap.sensor->name : "?",
                                    (unsigned)data_len);
                            }
                            s_session_had_alive = true;
                            // DIDs don't share the 8-bit PID-cache key
                            // space — DEAD-state for DIDs is in-RAM only
                            // (per session). Persistent DID-cache would
                            // need its own NVS namespace; deferred.
                        }
                    }
                } else if (s_inflight_proto == BRL_PROTO_UDS_BMW
                           && s_rx_len >= 3
                           && (s_rx_buf[0] == CMD_READ_DID_BMW
                               || s_rx_buf[0] == CMD_READ_DID_22)
                           && s_rx_buf[1] == STATUS_NEGATIVE) {
                    // Negative response from the ECU. Adapter format:
                    // [CMD][STATUS=0x03][NRC]. Common NRCs:
                    //   0x11 serviceNotSupported
                    //   0x12 subFunctionNotSupported
                    //   0x22 conditionsNotCorrect (e.g. engine not running)
                    //   0x31 requestOutOfRange (DID not supported)
                    //   0x33 securityAccessDenied
                    //   0x7E subFunctionNotSupportedInActiveSession
                    //   0x7F serviceNotSupportedInActiveSession
                    //     → either of these means we need extended session
                    //       (0x10 0x03) before the DID is readable
                    uint8_t nrc = (s_rx_len >= 3) ? s_rx_buf[2] : 0;
                    if (s_inflight_count == 1) {
                        int idx = s_inflight_idx[0];
                        if (idx >= 0 && idx < s_pid_count) {
                            ActivePid &ap = s_pids[idx];
                            ESP_LOGW(TAG,
                                "DID 0x%04X target=0x%02X NRC 0x%02X (%s)",
                                (unsigned)ap.did, (unsigned)ap.target,
                                (unsigned)nrc,
                                ap.sensor ? ap.sensor->name : "?");
                            // NRC counts as a strike — DDE clearly heard
                            // us, just refuses the DID. 3 strikes and it
                            // exits the round-robin so we don't keep
                            // burning cycles on it.
                            ap.strikes++;
                            if (ap.strikes >= DID_DEAD_THRESHOLD && !ap.dead) {
                                ap.dead = true;
                                ESP_LOGI(TAG,
                                    "DID 0x%04X dead after %d× NRC",
                                    (unsigned)ap.did, DID_DEAD_THRESHOLD);
                            }
                        }
                    }
                    // Don't fall through to the strike-everything-not-seen
                    // loop — we already handled this DID above.
                    s_inflight_count = 0;
                    s_inflight_proto = 0;
                    s_state = OBD_CONNECTED;
                    break;
                }

                // Strike everything we asked for that didn't come back.
                for (uint8_t i = 0; i < s_inflight_count; i++) {
                    if (ok && seen[i]) continue;
                    int idx = s_inflight_idx[i];
                    if (idx < 0 || idx >= s_pid_count) continue;
                    ActivePid &ap = s_pids[idx];
                    ap.strikes++;
                    uint8_t threshold = (ap.proto == BRL_PROTO_UDS_BMW)
                                        ? DID_DEAD_THRESHOLD
                                        : DEAD_THRESHOLD;
                    if (ap.strikes >= threshold && !ap.dead) {
                        ap.dead = true;
                        if (s_session_had_alive
                            && ap.proto == BRL_PROTO_OBD2_MODE1) {
                            // PID-keyed cache only fits Mode 01.
                            obd_pid_cache_set(ap.pid, PID_DEAD);
                        }
                        if (ap.proto == BRL_PROTO_OBD2_MODE1) {
                            ESP_LOGI(TAG, "PID 0x%02X dead after %d× no-answer",
                                     (unsigned)ap.pid, DEAD_THRESHOLD);
                        } else {
                            ESP_LOGI(TAG, "DID 0x%04X dead after %d× no-answer",
                                     (unsigned)ap.did, DID_DEAD_THRESHOLD);
                        }
                    }
                }
                s_inflight_count = 0;
                s_inflight_proto = 0;
                s_state = OBD_CONNECTED;
            } else {
                // Per-proto timeout. Mode-01 multi gets the long window
                // (BLE retransmit can stretch to ~700 ms on a flaky link)
                // and explicitly does NOT strike — striking 4 valid PIDs
                // every time a single request hangs would mass-murder the
                // round-robin. Mode-22 single-DID gets a short window
                // (when DDE answers it does so in <100 ms; if it didn't
                // answer in 300 ms it's not coming) and strikes the one
                // inflight DID. Without this, silent DIDs stay 'alive'
                // forever and starve real Mode-01 PIDs of polling cycles.
                bool timed_out = false;
                if (s_inflight_proto == BRL_PROTO_UDS_BMW) {
                    if (now - s_req_ts > DID_TIMEOUT_MS) {
                        timed_out = true;
                        ESP_LOGW(TAG, "DID 0x%04X timeout — no notify in %d ms",
                                 (unsigned)s_inflight_did, DID_TIMEOUT_MS);
                        // Strike the (single) inflight DID so dead Mode-22
                        // PIDs eventually exit the round-robin.
                        if (s_inflight_count == 1) {
                            int idx = s_inflight_idx[0];
                            if (idx >= 0 && idx < s_pid_count) {
                                ActivePid &ap = s_pids[idx];
                                ap.strikes++;
                                if (ap.strikes >= DID_DEAD_THRESHOLD
                                    && !ap.dead) {
                                    ap.dead = true;
                                    ESP_LOGI(TAG,
                                        "DID 0x%04X dead after %d× timeout",
                                        (unsigned)ap.did,
                                        DID_DEAD_THRESHOLD);
                                }
                            }
                        }
                    }
                } else if (now - s_req_ts > REQ_TIMEOUT_MS) {
                    timed_out = true;
                    ESP_LOGW(TAG, "Mode-01 multi-PID timeout — no notify in "
                                  "%d ms (%u PIDs in flight)",
                             REQ_TIMEOUT_MS, (unsigned)s_inflight_count);
                }
                if (timed_out) {
                    s_inflight_count = 0;
                    s_inflight_proto = 0;
                    s_state = OBD_CONNECTED;
                }
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

extern "C" const void *obd_bt_vehicle_profile(void)
{
    return s_vehicle_profile.loaded ? &s_vehicle_profile : nullptr;
}

extern "C" bool obd_bt_get_sensor_value(const void *sensor_v,
                                        float      *out_value,
                                        uint32_t   *out_age_ms)
{
    const CarSensor *target = (const CarSensor *)sensor_v;
    if (!target) return false;
    for (int i = 0; i < s_pid_count; i++) {
        if (s_pids[i].sensor == target) {
            if (!s_pids[i].has_value) return false;
            if (out_value)  *out_value  = s_pids[i].last_value;
            if (out_age_ms) *out_age_ms = millis() - s_pids[i].last_seen_ms;
            return true;
        }
    }
    return false;
}
