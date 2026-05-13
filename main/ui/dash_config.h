#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Field IDs -- what data a configurable slot displays
//
// Slot ID layout (8-bit, packed by range so the picker can switch on it):
//   0        FIELD_NONE        empty slot
//   1..  31  built-in          GPS speed, laptime, sectors, delta, map
//   32.. 63  legacy OBD        FIELD_RPM/THROTTLE/... — kept for back-compat
//                              with existing dash_config saves; resolves via
//                              the same dynamic-PID cache as the new range
//   64..127  reserved          (future built-ins)
//   128..191 OBD dynamic       index N → Universal-OBD2 sensor N (64 max)
//   192..255 CAN-direct        index N → active vehicle car_profile sensor N
//                              (motor-specific .brl, used in CAN_DIRECT mode)
//
// The dynamic ranges are populated at runtime from whichever .brl is loaded.
// Picker walks them in order, filtered by live-status from obd_dynamic /
// obd_pid_cache so the user only sees sensors this car actually answers.
// ---------------------------------------------------------------------------
typedef enum : uint8_t {
    FIELD_NONE      = 0,
    // Laptimer / GPS (Zone 1 & Zone 2 only)
    FIELD_SPEED     = 1,
    FIELD_LAPTIME   = 2,
    FIELD_BESTLAP   = 3,
    FIELD_DELTA_NUM = 4,
    FIELD_LAP_NR    = 5,
    FIELD_SECTOR1   = 6,
    FIELD_SECTOR2   = 7,
    FIELD_SECTOR3   = 8,
    FIELD_MAP       = 9,
    // OBD (Zone 3 only) — legacy hardcoded slots
    FIELD_RPM       = 32,
    FIELD_THROTTLE  = 33,
    FIELD_BOOST     = 34,
    FIELD_COOLANT   = 35,
    FIELD_INTAKE    = 36,
    FIELD_LAMBDA    = 37,
    FIELD_BRAKE     = 38,
    FIELD_STEERING  = 39,
    FIELD_BATTERY   = 40,    // PID 0x42 / "BattVolt" / Bordnetzspannung
    FIELD_MAF       = 41,    // PID 0x10 / Mass Air Flow (g/s)
    // Computed Live-Values (berechnet aus mehreren OBD2-PIDs, siehe
    // main/obd/computed_pids.cpp). Erscheinen im Sensor-Picker nur wenn die
    // benötigten Input-PIDs vom Auto unterstützt sind (Discovery-Bitmap).
    // FIELD_BOOST (oben, =34) wird intern auf computed_boost_kpa() gemappt,
    // also schon "intelligent": MAP-Baro wenn beide PIDs da, sonst leer.
    FIELD_AFR        = 49,   // Lambda × Stoich  — Stoich aus PID 0x51
    FIELD_POWER_KW   = 50,   // RPM × Torque / 9549   [kW]
    FIELD_POWER_PS   = 51,   // kW × 1.36             [PS]
    FIELD_TORQUE_NM  = 52,   // Load × RefTorque / 100 [Nm] (Fallback wenn 0x62 fehlt)
} FieldId;

// Dynamic range markers — slot IDs above these encode an index into the
// active .brl sensor list (128 + N for OBD-BLE, 192 + N for CAN-direct).
#define DASH_SLOT_OBD_DYN_BASE   128
#define DASH_SLOT_OBD_DYN_END    192
#define DASH_SLOT_CAN_DYN_BASE   192
#define DASH_SLOT_CAN_DYN_END    256

inline bool field_is_obd(uint8_t f)        { return f >= 32 && f < 64; }
inline bool field_is_obd_dynamic(uint8_t f){ return f >= DASH_SLOT_OBD_DYN_BASE && f < DASH_SLOT_OBD_DYN_END; }
inline bool field_is_can_dynamic(uint8_t f){ return f >= DASH_SLOT_CAN_DYN_BASE; }
inline uint8_t field_obd_dyn_index(uint8_t f) { return (uint8_t)(f - DASH_SLOT_OBD_DYN_BASE); }
inline uint8_t field_can_dyn_index(uint8_t f) { return (uint8_t)(f - DASH_SLOT_CAN_DYN_BASE); }

// Resolver helpers used by the slot renderer + picker UI. All four take
// the 8-bit slot ID and resolve through:
//   - built-in (laptime/sector/...)         → from g_state.timing/g_state.gps
//   - legacy FIELD_RPM..MAF                  → obd_dynamic_get(corresponding_pid)
//   - dynamic OBD (128+N)                    → Universal-OBD2 sensor[N]'s PID → obd_dynamic_get
//   - dynamic CAN-direct (192+N)             → g_car_profile.sensors[N] (decoded by can_bus)
//
// The resolvers know the sensor unit/scale/format from the .brl, so all
// three frontends (display / Studio / Android) get identical text.

#ifdef __cplusplus
extern "C" {
#endif

// Human-readable title for the slot picker label and slot title bar.
// Returns a constant string (do not free). For unknown slots: "?".
const char *field_title(uint8_t slot);

// Format the live value for `slot` into `out` as a printable string
// (e.g. "1850", "92.3", "--"). Returns true on success, false if the
// slot has no value yet (caller renders "--"). `width` hints maximum
// digits (used by the dash to pick between fonts).
bool field_format_value(uint8_t slot, char *out, int out_size);

// True when the slot is currently producing fresh data (within the
// last few seconds). Used by the picker for the live-badge filter
// and by the dash to grey-out stale slots.
bool field_is_live(uint8_t slot);

// Unit string ("rpm", "°C", "kPa", "%", ...). Empty string if none.
const char *field_unit(uint8_t slot);

#ifdef __cplusplus
}
#endif

// Slot counts per zone
#define Z1_SLOTS  5   // Zone 1: slots 0-1 wide, 2-4 narrow
#define Z2_SLOTS  3   // Zone 2: sectors
#define Z3_SLOTS  5   // Zone 3: OBD

typedef struct {
    uint8_t language;        // 0 = Deutsch, 1 = English
    uint8_t units;           // 0 = metric (km/h), 1 = imperial (mph)
    uint16_t delta_scale_ms; // Delta bar scale in ms (2000/3000/5000/10000/20000)
    uint8_t z1[Z1_SLOTS];
    uint8_t z2[Z2_SLOTS];
    uint8_t z3[Z3_SLOTS];
    uint8_t veh_conn_mode;   // 0 = OBD BLE, 1 = CAN direct
    uint8_t show_obd;        // 0 = hide zone-3 engine-data widgets, 1 = show
    uint8_t brightness;      // LCD backlight 10..100 (%). Floored at 10 in
                             // the settings UI so a misclick can't blank the
                             // display — the only way back from 0 is a reflash.
} DashConfig;

extern DashConfig g_dash_cfg;

void dash_config_load();
void dash_config_save();
