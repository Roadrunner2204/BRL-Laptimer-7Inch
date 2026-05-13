/**
 * dash_config.cpp -- Dashboard configuration persistence via NVS
 *
 * Stores the DashConfig struct (language, units, zone slot assignments)
 * in NVS namespace "dashcfg".
 *
 * Keys:
 *   "lang"   u8
 *   "units"  u8
 *   "z1"     blob (Z1_SLOTS bytes)
 *   "z2"     blob (Z2_SLOTS bytes)
 *   "z3"     blob (Z3_SLOTS bytes)
 */

#include "dash_config.h"
#include "compat.h"
#include "i18n.h"
#include "../data/lap_data.h"
#include "../data/car_profile.h"
#include "../obd/obd_dynamic.h"
#include "../obd/obd_bt.h"
#include "../obd/computed_pids.h"

#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "dash_config";

#define NVS_NAMESPACE "dashcfg"

// ---------------------------------------------------------------------------
// Global dashboard configuration
// ---------------------------------------------------------------------------
DashConfig g_dash_cfg = {};

// ---------------------------------------------------------------------------
// Default configuration
// ---------------------------------------------------------------------------
static const DashConfig DEFAULT_CFG = {
    .language       = 0,     // Deutsch
    .units          = 0,     // km/h
    .delta_scale_ms = 3000,  // ±3 s

    // Zone 1: [Speed, Laptime, Bestlap, Delta, LapNr]
    .z1 = { FIELD_SPEED, FIELD_LAPTIME, FIELD_BESTLAP, FIELD_DELTA_NUM, FIELD_LAP_NR },

    // Zone 2: [Sector1, Sector2, Sector3]
    .z2 = { FIELD_SECTOR1, FIELD_SECTOR2, FIELD_SECTOR3 },

    // Zone 3: [RPM, Throttle, Boost, Coolant, None]
    .z3 = { FIELD_RPM, FIELD_THROTTLE, FIELD_BOOST, FIELD_COOLANT, FIELD_NONE },

    .veh_conn_mode = 0,  // OBD BLE default
    .show_obd      = 1,  // engine-data widgets visible by default
    .brightness    = 100,
};

// ---------------------------------------------------------------------------
// dash_config_load -- read config from NVS, or use defaults
// ---------------------------------------------------------------------------
void dash_config_load()
{
    // Start with defaults
    g_dash_cfg = DEFAULT_CFG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        log_i("NVS '%s' not found (%s), using defaults",
              NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }

    nvs_get_u8(h, "lang",  &g_dash_cfg.language);
    nvs_get_u8(h, "units", &g_dash_cfg.units);
    nvs_get_u16(h, "dscale", &g_dash_cfg.delta_scale_ms);
    nvs_get_u8(h, "vconn", &g_dash_cfg.veh_conn_mode);
    nvs_get_u8(h, "shobd", &g_dash_cfg.show_obd);
    nvs_get_u8(h, "bright", &g_dash_cfg.brightness);
    if (g_dash_cfg.brightness < 10 || g_dash_cfg.brightness > 100) {
        g_dash_cfg.brightness = 100;
    }

    size_t len;

    len = sizeof(g_dash_cfg.z1);
    if (nvs_get_blob(h, "z1", g_dash_cfg.z1, &len) != ESP_OK)
        memcpy(g_dash_cfg.z1, DEFAULT_CFG.z1, sizeof(g_dash_cfg.z1));

    len = sizeof(g_dash_cfg.z2);
    if (nvs_get_blob(h, "z2", g_dash_cfg.z2, &len) != ESP_OK)
        memcpy(g_dash_cfg.z2, DEFAULT_CFG.z2, sizeof(g_dash_cfg.z2));

    len = sizeof(g_dash_cfg.z3);
    if (nvs_get_blob(h, "z3", g_dash_cfg.z3, &len) != ESP_OK)
        memcpy(g_dash_cfg.z3, DEFAULT_CFG.z3, sizeof(g_dash_cfg.z3));

    nvs_close(h);

    log_i("Config loaded: lang=%u units=%u z1=[%u,%u,%u,%u,%u]",
          g_dash_cfg.language, g_dash_cfg.units,
          g_dash_cfg.z1[0], g_dash_cfg.z1[1], g_dash_cfg.z1[2],
          g_dash_cfg.z1[3], g_dash_cfg.z1[4]);
}

// ---------------------------------------------------------------------------
// dash_config_save -- persist config to NVS
// ---------------------------------------------------------------------------
void dash_config_save()
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        log_e("nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }

    nvs_set_u8(h, "lang",  g_dash_cfg.language);
    nvs_set_u8(h, "units", g_dash_cfg.units);
    nvs_set_u16(h, "dscale", g_dash_cfg.delta_scale_ms);
    nvs_set_u8(h, "vconn", g_dash_cfg.veh_conn_mode);
    nvs_set_u8(h, "shobd", g_dash_cfg.show_obd);
    nvs_set_u8(h, "bright", g_dash_cfg.brightness);
    nvs_set_blob(h, "z1", g_dash_cfg.z1, sizeof(g_dash_cfg.z1));
    nvs_set_blob(h, "z2", g_dash_cfg.z2, sizeof(g_dash_cfg.z2));
    nvs_set_blob(h, "z3", g_dash_cfg.z3, sizeof(g_dash_cfg.z3));

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        log_e("nvs_commit failed: %s", esp_err_to_name(err));
    } else {
        log_i("Config saved");
    }
}

// ===========================================================================
// Slot resolvers — title, value, unit, live-status for any 8-bit slot ID
//
// One function per concern, all four switch on the slot range first, then
// dispatch:
//   - hardcoded built-ins (laptime/sector/...)
//   - legacy FIELD_RPM..MAF (resolved through their well-known PID)
//   - dynamic OBD (128+N) — looks up Universal-OBD2[N], reads obd_dynamic[pid]
//   - dynamic CAN (192+N) — looks up g_car_profile.sensors[N] (currently
//                            decoded into g_state.obd by can_bus.cpp; until
//                            that grows a per-sensor cache, we reuse the
//                            sensor name to map back into legacy fields)
// ===========================================================================

// PID byte that backs each legacy FIELD_X. -1 if no direct PID mapping
// (e.g. FIELD_BRAKE/STEERING come from external inputs, not a PID).
static int legacy_field_pid(uint8_t f)
{
    switch (f) {
        case FIELD_RPM:      return 0x0C;
        case FIELD_THROTTLE: return 0x11;
        // FIELD_BOOST wurde entfernt — MAP-Baro Berechnung läuft über
        // computed_boost_kpa() in field_format_value, damit der angezeigte
        // Wert wirklich relativer Ladedruck ist (nicht absoluter MAP).
        case FIELD_COOLANT:  return 0x05;
        case FIELD_INTAKE:   return 0x0F;
        case FIELD_LAMBDA:   return 0x24;
        case FIELD_BATTERY:  return 0x42;
        case FIELD_MAF:      return 0x10;
        default:             return -1;
    }
}

// Find a CarProfile pointer either from the OBD.brl loaded in obd_bt or
// the active CAN-direct car profile.
static const CarProfile *obd_brl_profile(void)
{
    return (const CarProfile *)obd_bt_pid_profile();
}

const char *field_title(uint8_t slot)
{
    if (slot == FIELD_NONE) return "---";

    // Built-ins
    switch (slot) {
        case FIELD_SPEED:     return tr(TR_SPEED);
        case FIELD_LAPTIME:   return tr(TR_LAPTIME);
        case FIELD_BESTLAP:   return tr(TR_BESTLAP);
        case FIELD_DELTA_NUM: return tr(TR_LIVE_DELTA);
        case FIELD_LAP_NR:    return tr(TR_LAP);
        case FIELD_SECTOR1:   return tr(TR_SECTOR1);
        case FIELD_SECTOR2:   return tr(TR_SECTOR2);
        case FIELD_SECTOR3:   return tr(TR_SECTOR3);
        case FIELD_MAP:       return "MAP";
        case FIELD_RPM:       return tr(TR_RPM);
        case FIELD_THROTTLE:  return tr(TR_THROTTLE);
        case FIELD_BOOST:     return tr(TR_BOOST);
        case FIELD_LAMBDA:    return tr(TR_LAMBDA);
        case FIELD_BRAKE:     return tr(TR_BRAKE);
        case FIELD_COOLANT:   return tr(TR_COOLANT);
        case FIELD_INTAKE:    return tr(TR_INTAKE);
        case FIELD_STEERING:  return tr(TR_STEERING);
        case FIELD_BATTERY:   return "Batterie";
        case FIELD_MAF:       return "MAF";
        case FIELD_AFR:       return "AFR";
        case FIELD_POWER_KW:  return "Power";
        case FIELD_POWER_PS:  return "Power";
        case FIELD_TORQUE_NM: return (i18n_get_language() == 0) ? "Drehmoment" : "Torque";
        default: break;
    }

    if (field_is_obd_dynamic(slot)) {
        // OBD.brl first, then vehicle.brl — same packing as
        // build_z3_fields and field_format_value above.
        uint8_t idx = field_obd_dyn_index(slot);
        const CarProfile *obd_p = obd_brl_profile();
        const CarProfile *veh_p = (const CarProfile *)obd_bt_vehicle_profile();
        int obd_n = obd_p ? obd_p->sensor_count : 0;
        if (idx < obd_n) return obd_p->sensors[idx].name;
        if (veh_p && (idx - obd_n) < veh_p->sensor_count)
            return veh_p->sensors[idx - obd_n].name;
        return "?";
    }
    if (field_is_can_dynamic(slot)) {
        uint8_t idx = field_can_dyn_index(slot);
        if (g_car_profile.loaded && idx < g_car_profile.sensor_count)
            return g_car_profile.sensors[idx].name;
        return "?";
    }
    return "---";
}

const char *field_unit(uint8_t slot)
{
    // Legacy + dynamic both look up by name/PID context. We don't store
    // unit strings in CarSensor (CAN-Checked .brl format has none), so
    // we map by sensor name + PID range. Keep a tight whitelist —
    // anything unknown returns "" so the dash doesn't show garbage.
    if (slot == FIELD_RPM)       return "rpm";
    if (slot == FIELD_THROTTLE)  return "%";
    if (slot == FIELD_BOOST)     return "kPa";
    if (slot == FIELD_COOLANT)   return "°C";
    if (slot == FIELD_INTAKE)    return "°C";
    if (slot == FIELD_LAMBDA)    return "λ";
    if (slot == FIELD_BATTERY)   return "V";
    if (slot == FIELD_MAF)       return "g/s";
    if (slot == FIELD_AFR)       return ":1";   // AFR ist Massenverhältnis (z.B. 14.7:1 stoich Benzin)
    if (slot == FIELD_POWER_KW)  return "kW";
    if (slot == FIELD_POWER_PS)  return "PS";
    if (slot == FIELD_TORQUE_NM) return "Nm";
    if (slot == FIELD_SPEED)     return (g_dash_cfg.units == 0) ? "km/h" : "mph";
    if (field_is_obd_dynamic(slot)) {
        uint8_t idx = field_obd_dyn_index(slot);
        const CarProfile *obd_p = obd_brl_profile();
        const CarProfile *veh_p = (const CarProfile *)obd_bt_vehicle_profile();
        int obd_n = obd_p ? obd_p->sensor_count : 0;
        const CarSensor *s = nullptr;
        if (idx < obd_n) s = &obd_p->sensors[idx];
        else if (veh_p && (idx - obd_n) < veh_p->sensor_count)
            s = &veh_p->sensors[idx - obd_n];
        if (!s) return "";
        // CAN-Checked .brl stores `type`: 1=pressure 2=temp 3=speed 4=lambda.
        switch (s->type) {
            case 1: return "kPa";
            case 2: return "°C";
            case 3: return (g_dash_cfg.units == 0) ? "km/h" : "mph";
            case 4: return "λ";
            default: return "";
        }
    }
    if (field_is_can_dynamic(slot)) {
        uint8_t idx = field_can_dyn_index(slot);
        if (!g_car_profile.loaded || idx >= g_car_profile.sensor_count) return "";
        switch (g_car_profile.sensors[idx].type) {
            case 1: return "kPa";
            case 2: return "°C";
            case 3: return (g_dash_cfg.units == 0) ? "km/h" : "mph";
            case 4: return "λ";
            default: return "";
        }
    }
    return "";
}

bool field_is_live(uint8_t slot)
{
    // Built-ins are always "live" (timing/GPS is always up).
    if (slot < 32) return slot != FIELD_NONE;

    // Legacy OBD field → check the corresponding PID's freshness.
    int legacy_pid = legacy_field_pid(slot);
    if (legacy_pid >= 0) {
        return obd_dynamic_is_live((uint8_t)legacy_pid);
    }

    // Dynamic OBD slot — proto-agnostic via per-sensor cache age.
    if (field_is_obd_dynamic(slot)) {
        uint8_t idx = field_obd_dyn_index(slot);
        const CarProfile *obd_p = obd_brl_profile();
        const CarProfile *veh_p = (const CarProfile *)obd_bt_vehicle_profile();
        int obd_n = obd_p ? obd_p->sensor_count : 0;
        const CarSensor *s = nullptr;
        if (idx < obd_n) s = &obd_p->sensors[idx];
        else if (veh_p && (idx - obd_n) < veh_p->sensor_count)
            s = &veh_p->sensors[idx - obd_n];
        if (!s) return false;
        uint32_t age = 0;
        if (!obd_bt_get_sensor_value(s, nullptr, &age)) return false;
        return age <= OBD_DYNAMIC_LIVE_WINDOW_MS;
    }

    // CAN-direct dynamic — for now we don't have a per-sensor freshness
    // tracker on the CAN bus path; assume live whenever the bus is up.
    if (field_is_can_dynamic(slot)) {
        uint8_t idx = field_can_dyn_index(slot);
        return g_car_profile.loaded && idx < g_car_profile.sensor_count;
    }

    return false;
}

bool field_format_value(uint8_t slot, char *out, int out_size)
{
    if (!out || out_size < 2) return false;
    out[0] = '\0';

    // Computed Live-Values (berechnet aus mehreren PIDs).
    // FIELD_BOOST geht hier auch durch — der "richtige" Boost ist MAP-Baro,
    // nicht der direkte Wert von PID 0x0B (das ist absoluter MAP).
    auto fmt_computed = [&](float v, int decimals) {
        if (decimals <= 0) snprintf(out, out_size, "%.0f", (double)v);
        else               snprintf(out, out_size, "%.*f", decimals, (double)v);
    };
    {
        float v;
        switch (slot) {
            case FIELD_BOOST:
                if (computed_boost_kpa(&v))   { fmt_computed(v, 0); return true; }
                return false;
            case FIELD_AFR:
                if (computed_afr(&v))         { fmt_computed(v, 1); return true; }
                return false;
            case FIELD_POWER_KW:
                if (computed_power_kw(&v))    { fmt_computed(v, 0); return true; }
                return false;
            case FIELD_POWER_PS:
                if (computed_power_ps(&v))    { fmt_computed(v, 0); return true; }
                return false;
            case FIELD_TORQUE_NM:
                if (computed_torque_nm(&v))   { fmt_computed(v, 0); return true; }
                return false;
            default: break;
        }
    }

    // Legacy / dynamic OBD → resolve PID + read obd_dynamic.
    auto fmt_pid = [&](uint8_t pid, int decimals) -> bool {
        float v;
        if (!obd_dynamic_get(pid, &v)) return false;
        if (decimals <= 0)
            snprintf(out, out_size, "%.0f", (double)v);
        else
            snprintf(out, out_size, "%.*f", decimals, (double)v);
        return true;
    };

    int legacy_pid = legacy_field_pid(slot);
    if (legacy_pid >= 0) {
        // Decimals follow the typical display precision for each.
        int dec = (slot == FIELD_LAMBDA || slot == FIELD_BATTERY) ? 2
                : (slot == FIELD_MAF) ? 1
                : 0;
        return fmt_pid((uint8_t)legacy_pid, dec);
    }

    if (field_is_obd_dynamic(slot)) {
        // Dynamic slot — index into either OBD.brl (low half of the
        // dynamic range) or the active vehicle profile (high half).
        // The picker emits indices into a virtual "OBD.brl + vehicle.brl"
        // catalog: first all OBD.brl sensors, then vehicle-profile
        // sensors. We mirror that order here so a saved slot ID remains
        // stable across reboots.
        uint8_t idx = field_obd_dyn_index(slot);
        const CarProfile *obd_p = obd_brl_profile();
        const CarProfile *veh_p = (const CarProfile *)obd_bt_vehicle_profile();
        const CarSensor *s = nullptr;
        int obd_n = obd_p ? obd_p->sensor_count : 0;
        if (idx < obd_n) {
            s = &obd_p->sensors[idx];
        } else if (veh_p && (idx - obd_n) < veh_p->sensor_count) {
            s = &veh_p->sensors[idx - obd_n];
        }
        if (!s) return false;

        // Read the per-sensor live value (proto-agnostic — works for
        // Mode 01 PIDs and Mode 22 DIDs alike). Falls back to the global
        // PID-keyed cache when this is a sensor without a live record
        // yet but its underlying PID has been seen elsewhere (handles
        // duplicate PIDs across OBD.brl + vehicle.brl).
        float v;
        bool have = obd_bt_get_sensor_value(s, &v, nullptr);
        if (!have && s->proto == 7) {
            uint8_t pid = (uint8_t)(s->can_id & 0xFFu);
            have = obd_dynamic_get(pid, &v);
        }
        if (!have) return false;

        // Decimals from .brl scale (high precision → more decimals).
        int dec = 0;
        float as = fabsf(s->scale);
        if (as < 0.001f) dec = 4;
        else if (as < 0.01f) dec = 3;
        else if (as < 0.1f)  dec = 2;
        else if (as < 1.0f)  dec = 1;
        if (dec <= 0) snprintf(out, out_size, "%.0f", (double)v);
        else          snprintf(out, out_size, "%.*f", dec, (double)v);
        return true;
    }

    if (field_is_can_dynamic(slot)) {
        // CAN-direct path uses g_car_profile + can_bus.cpp's own per-
        // sensor write into g_state.obd.X. There's no proto-agnostic
        // live cache for CAN-direct yet — when the user runs the
        // hardwired CAN mode, can_bus needs a parallel
        // obd_bt_get_sensor_value-style API. Returning false here is
        // safe (slot renders "--") and the legacy obd.* fields still
        // populate via the FIELD_RPM/FIELD_THROTTLE etc. shortcut.
        return false;
    }

    // Built-ins (timing / sectors / delta) are formatted by the timing
    // screen itself — it has all the LiveTiming context. dash_config
    // doesn't try to second-guess that. Caller for non-OBD slots should
    // fall through to its own switch.
    return false;
}
