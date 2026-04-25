#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../data/lap_data.h"

/**
 * analog_in -- 4 single-ended ADC1 inputs on the Waveshare ESP32-P4 header.
 *
 * GPIO mapping (only ADC1 channels free of any other use on the board):
 *   AN1 = GPIO 20   ADC1 channel 4
 *   AN2 = GPIO 21   ADC1 channel 5
 *   AN3 = GPIO 22   ADC1 channel 6
 *   AN4 = GPIO 23   ADC1 channel 7
 *
 * Reference:  3.3 V (the chip's internal Vref via curve calibration).
 * Range:      0 .. 3300 mV  (single-ended, 12 dB attenuation = 0..3.1 V usable;
 *             a small headroom is always lost at the rail, this is normal).
 * Sample rate: ~10 Hz (one read per channel inside analog_in_poll()).
 *
 * Calibration per channel: linear   value = raw_mv * scale + offset
 *   - default scale=1.0, offset=0.0  -> value === raw_mv
 *   - example pressure sensor 0.5 V = 0 bar / 4.5 V = 10 bar:
 *       scale = 10 / (4500-500) = 0.0025
 *       offset = -500 * 0.0025 = -1.25
 *
 * Persistence: name + scale + offset + min/max are NVS-backed (namespace
 * "analog"). Values are wired into g_state.analog[i] for any UI/storage
 * code to consume just like OBD data.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define ANALOG_NAME_LEN  16

typedef struct {
    char     name[ANALOG_NAME_LEN]; // user label, e.g. "Oilpres"
    float    scale;                 // linear gain (mV -> physical units)
    float    offset;                // linear bias (after scale)
    float    min_val;               // clamp lower bound on the calibrated value
    float    max_val;               // clamp upper bound on the calibrated value
    bool     enabled;               // false = channel inactive (no ADC read)
} AnalogChannelCfg;

extern AnalogChannelCfg g_analog_cfg[ANALOG_CHANNELS];

/// One-time hardware setup. Safe to call before SD/NVS are mounted; the
/// configuration is loaded later (analog_in_load_config) once NVS is up.
void analog_in_init(void);

/// Read the persisted per-channel configuration from NVS into g_analog_cfg.
/// Defaults are applied for any channel not yet stored.
void analog_in_load_config(void);

/// Persist the current g_analog_cfg back to NVS. Call after the user
/// edits a channel from the settings screen.
void analog_in_save_config(void);

/// Sample all enabled channels and write into g_state.analog[]. Designed
/// to be called from the 10 Hz logic timer.
void analog_in_poll(void);

#ifdef __cplusplus
}
#endif
