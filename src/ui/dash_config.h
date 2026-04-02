#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Widget bitmask IDs
// Each bit controls visibility of one timing-screen widget.
// ---------------------------------------------------------------------------
#define WDGT_SPEED       (1UL<<0)   // GPS speed           (large, 2-col)
#define WDGT_LAPTIME     (1UL<<1)   // current lap time    (large, 2-col)
#define WDGT_BESTLAP     (1UL<<2)   // best lap time
#define WDGT_DELTA       (1UL<<3)   // live Δ vs reference
#define WDGT_LAP_NR      (1UL<<4)   // lap counter
#define WDGT_SECTOR1     (1UL<<5)   // sector 1
#define WDGT_SECTOR2     (1UL<<6)   // sector 2
#define WDGT_SECTOR3     (1UL<<7)   // sector 3
#define WDGT_RPM         (1UL<<8)   // engine RPM
#define WDGT_THROTTLE    (1UL<<9)   // throttle %
#define WDGT_BOOST       (1UL<<10)  // boost / MAP kPa
#define WDGT_LAMBDA      (1UL<<11)  // lambda / AFR
#define WDGT_BRAKE       (1UL<<12)  // brake pedal %
#define WDGT_COOLANT     (1UL<<13)  // coolant temp
#define WDGT_GEAR        (1UL<<14)  // gear
#define WDGT_STEERING    (1UL<<15)  // steering angle

// Default: show the most useful widgets
#define WDGT_DEFAULT_MASK  (WDGT_SPEED | WDGT_LAPTIME | WDGT_BESTLAP | \
                            WDGT_DELTA | WDGT_LAP_NR | \
                            WDGT_SECTOR1 | WDGT_SECTOR2 | WDGT_SECTOR3 | \
                            WDGT_RPM | WDGT_THROTTLE | WDGT_BOOST)

// ---------------------------------------------------------------------------
// Config struct (one global instance)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t visible_mask;
} DashConfig;

extern DashConfig g_dash_cfg;

// Load from NVS (called once at startup)
void dash_config_load();

// Save to NVS (called after layout editor saves)
void dash_config_save();
