#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * computed_pids — Berechnete Werte aus Standard-OBD2-PIDs.
 *
 * Alle Berechnungen sind "passiv": obd_bt.cpp ruft computed_update() für
 * jeden eingehenden Sensor-Wert auf, dieses Modul speichert die letzten
 * Inputs intern und liefert auf Anfrage die Berechnung. Wenn Inputs fehlen
 * (= ECU unterstützt diese PID nicht) → Getter liefert false.
 *
 * Berechnete Werte (Torque-Pro-Style):
 *   - Boost (relativ)  = MAP - Baro                   [kPa]
 *   - AFR              = Lambda × Stoich-Faktor       [ratio]
 *                        Stoich-Faktor wird aus PID 0x51 (FuelType) abgeleitet.
 *                        Default Benzin (14.7) wenn ECU 0x51 nicht hat.
 *   - Power (kW)       = RPM × Torque / 9549          [kW]
 *                        Nutzt PID 0x62 (Actual Torque %) × 0x63 (RefTorque)
 *                        wenn beide vorhanden, sonst Load-basiert (Fallback).
 *   - Power (PS)       = kW × 1.36                    [PS]
 *   - Torque (calc)    = (Load × RefTorque) / 100     [Nm]
 *                        Fallback wenn 0x62 fehlt aber 0x04 + 0x63 da sind.
 */

#ifdef __cplusplus
extern "C" {
#endif

// Bei jedem decoded Sensor-Wert aus obd_bt.cpp aufrufen — Modul behält
// nur die Werte die es für Berechnungen braucht.
//
// Erkannte sensor-Namen (case-sensitive, aus Universal-OBD.brl):
//   "RPM", "MAP", "Baro", "Load", "Lambda" (oder "O2_B1S1"), "ActualTrq",
//   "RefTrq", "FuelType"
void computed_update(const char *sensor_name, float value);

// Disconnect / Auto-Wechsel — alle gespeicherten Werte invalidieren.
void computed_reset(void);

// Getter — geben true zurück wenn alle benötigten Inputs jemals empfangen
// wurden, false sonst. Wert wird in *out geschrieben.
bool computed_boost_kpa(float *out);     // MAP - Baro
bool computed_afr(float *out);            // Lambda × Stoich
bool computed_power_kw(float *out);       // RPM × Torque/9549
bool computed_power_ps(float *out);       // kW × 1.36
bool computed_torque_nm(float *out);      // Load × RefTorque / 100  (Fallback)

// Aktiver Stoich-Faktor (für UI / Diagnose). 14.7 = Benzin Default.
float computed_stoich_factor(void);

#ifdef __cplusplus
}
#endif
