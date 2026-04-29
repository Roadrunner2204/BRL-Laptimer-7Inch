#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Field IDs -- what data a configurable slot displays
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
    // OBD (Zone 3 only)
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
    // Analog inputs (Zone 3, alongside OBD). One per ADC1 channel on
    // GPIO 20/21/22/23. Value comes from g_state.analog[i].value (already
    // scale+offset+clamp applied by analog_in_poll).
    FIELD_AN1       = 64,
    FIELD_AN2       = 65,
    FIELD_AN3       = 66,
    FIELD_AN4       = 67,
} FieldId;

inline bool field_is_obd(uint8_t f) { return f >= 32 && f < 64; }
inline bool field_is_analog(uint8_t f) { return f >= 64 && f < 68; }

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
} DashConfig;

extern DashConfig g_dash_cfg;

void dash_config_load();
void dash_config_save();
