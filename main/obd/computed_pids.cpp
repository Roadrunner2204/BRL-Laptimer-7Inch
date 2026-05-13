// =============================================================================
//  computed_pids — Live-Berechnungen aus Standard-OBD2-PID-Werten
// =============================================================================
#include "computed_pids.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"

static const char *TAG = "computed";

// Letzte empfangene Werte. NaN = "noch nie empfangen".
static float s_rpm        = NAN;
static float s_map_kpa    = NAN;
static float s_baro_kpa   = NAN;
static float s_load_pct   = NAN;
static float s_lambda     = NAN;
static float s_torque_pct = NAN;   // Actual Torque % (PID 0x62)
static float s_torque_ref = NAN;   // Reference Torque Nm (PID 0x63)

// Stoich-Faktor laut Fuel-Type (PID 0x51). Default Benzin wenn nie erhalten.
// Werte aus SAE J1979 Table 16: Fuel Type encoding.
static float s_stoich = 14.7f;     // Benzin Default

static float stoich_for_fuel_type(uint8_t fuel_type) {
    switch (fuel_type) {
        case 1:  return 14.7f;   // Gasoline
        case 2:  return 6.4f;    // Methanol
        case 3:  return 9.0f;    // Ethanol
        case 4:  return 14.5f;   // Diesel
        case 5:  return 15.5f;   // LPG
        case 6:  return 17.2f;   // CNG
        case 7:  return 15.7f;   // Propane
        // Bifuel laufend → wir nehmen den Modus an dem die ECU gerade ist
        case 9:  return 14.7f;   // Bifuel running Gasoline
        case 10: return 6.4f;    // Bifuel running Methanol
        case 11: return 9.0f;    // Bifuel running Ethanol
        case 12: return 15.5f;   // Bifuel running LPG
        case 13: return 17.2f;   // Bifuel running CNG
        case 14: return 15.7f;   // Bifuel running Propane
        // Hybrid combustion engine — primary fuel
        case 17: return 14.7f;   // Hybrid Gasoline
        case 18: return 9.0f;    // Hybrid Ethanol
        case 19: return 14.5f;   // Hybrid Diesel
        case 23: return 14.5f;   // Bifuel running Diesel
        // 8 (Electric), 16/20/21/22 (Pure-Hybrid/Regenerative): kein Verbrenner aktiv
        // → AFR ist nicht sinnvoll, wir setzen 14.7 als harmlosen Default.
        default: return 14.7f;
    }
}

void computed_reset(void) {
    s_rpm = s_map_kpa = s_baro_kpa = s_load_pct = s_lambda = NAN;
    s_torque_pct = s_torque_ref = NAN;
    s_stoich = 14.7f;
}

void computed_update(const char *sensor_name, float value) {
    if (!sensor_name) return;
    // Inputs erkennen — case-sensitiv, Namen aus Universal-OBD.brl
    if      (strcmp(sensor_name, "RPM")        == 0) s_rpm        = value;
    else if (strcmp(sensor_name, "MAP")        == 0) s_map_kpa    = value;
    else if (strcmp(sensor_name, "Baro")       == 0) s_baro_kpa   = value;
    else if (strcmp(sensor_name, "Load")       == 0) s_load_pct   = value;
    // Lambda: nehme den ersten verfügbaren Wert. Reihenfolge spielt keine
    // Rolle wenn das Auto Wide-Band-Lambda hat (Bank1 Sensor1 ist meist
    // der Hauptsensor).
    else if (strcmp(sensor_name, "Lambda")     == 0) s_lambda     = value;
    else if (strcmp(sensor_name, "O2_B1S1_V")  == 0 && isnan(s_lambda)) {
        // Narrow-band Lambda-Spannung (V): grobe Umrechnung 0.5V = stoich
        // = Lambda 1.0. Nicht präzise aber besser als nichts.
        s_lambda = value * 2.0f;
    }
    else if (strcmp(sensor_name, "ActualTrq")  == 0) s_torque_pct = value;
    else if (strcmp(sensor_name, "RefTrq")     == 0) s_torque_ref = value;
    else if (strcmp(sensor_name, "FuelType")   == 0) {
        uint8_t ft = (uint8_t)value;
        float new_stoich = stoich_for_fuel_type(ft);
        if (new_stoich != s_stoich) {
            ESP_LOGI(TAG, "Fuel-Type 0x%02X erkannt → AFR-Stoich = %.2f",
                     (unsigned)ft, (double)new_stoich);
            s_stoich = new_stoich;
        }
    }
}

bool computed_boost_kpa(float *out) {
    if (isnan(s_map_kpa)) return false;
    // Wenn Baro vorhanden: exakte Berechnung. Sonst Annahme 101.3 kPa NN.
    float baro = isnan(s_baro_kpa) ? 101.3f : s_baro_kpa;
    if (out) *out = s_map_kpa - baro;
    return true;
}

bool computed_afr(float *out) {
    if (isnan(s_lambda)) return false;
    if (out) *out = s_lambda * s_stoich;
    return true;
}

bool computed_power_kw(float *out) {
    if (isnan(s_rpm) || isnan(s_torque_ref)) return false;
    // Bevorzugt: ActualTrq (PID 0x62) als signed Prozent von RefTrq
    // Fallback: Load × RefTorque (weniger genau)
    float trq_nm;
    if (!isnan(s_torque_pct)) {
        trq_nm = s_torque_pct * s_torque_ref / 100.0f;   // bereits "actual" %
    } else if (!isnan(s_load_pct)) {
        trq_nm = s_load_pct * s_torque_ref / 100.0f;
    } else {
        return false;
    }
    if (out) *out = s_rpm * trq_nm / 9549.0f;
    return true;
}

bool computed_power_ps(float *out) {
    float kw;
    if (!computed_power_kw(&kw)) return false;
    if (out) *out = kw * 1.35962f;       // 1 kW = 1.35962 PS
    return true;
}

bool computed_torque_nm(float *out) {
    if (isnan(s_torque_ref)) return false;
    if (!isnan(s_torque_pct)) {
        // Direkter Wert aus PID 0x62 × 0x63
        if (out) *out = s_torque_pct * s_torque_ref / 100.0f;
        return true;
    }
    if (!isnan(s_load_pct)) {
        // Load-Fallback (PID 0x04 × 0x63), weniger genau
        if (out) *out = s_load_pct * s_torque_ref / 100.0f;
        return true;
    }
    return false;
}

float computed_stoich_factor(void) { return s_stoich; }
