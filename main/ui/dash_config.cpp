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

#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

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
