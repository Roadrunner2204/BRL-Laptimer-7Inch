/**
 * dash_config.cpp — Dashboard configuration (NVS persistence)
 *
 * Ported from Arduino Preferences to ESP-IDF NVS.
 */

#include "dash_config.h"
#include "compat.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "dash_config";

DashConfig g_dash_cfg = {
    /* language */ 0,
    /* units    */ 0,
    /* z1 */ { FIELD_SPEED, FIELD_LAPTIME, FIELD_BESTLAP, FIELD_DELTA_NUM, FIELD_LAP_NR },
    /* z2 */ { FIELD_SECTOR1, FIELD_SECTOR2, FIELD_SECTOR3 },
    /* z3 */ { FIELD_RPM, FIELD_THROTTLE, FIELD_BOOST, FIELD_COOLANT, FIELD_NONE },
};

void dash_config_load() {
    nvs_handle_t h;
    if (nvs_open("dashcfg", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config — using defaults");
        return;
    }

    nvs_get_u8(h, "lang",  &g_dash_cfg.language);
    nvs_get_u8(h, "units", &g_dash_cfg.units);

    size_t len = Z1_SLOTS;
    nvs_get_blob(h, "z1", g_dash_cfg.z1, &len);
    len = Z2_SLOTS;
    nvs_get_blob(h, "z2", g_dash_cfg.z2, &len);
    len = Z3_SLOTS;
    nvs_get_blob(h, "z3", g_dash_cfg.z3, &len);

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded");
}

void dash_config_save() {
    nvs_handle_t h;
    if (nvs_open("dashcfg", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }

    nvs_set_u8(h, "lang",  g_dash_cfg.language);
    nvs_set_u8(h, "units", g_dash_cfg.units);
    nvs_set_blob(h, "z1", g_dash_cfg.z1, Z1_SLOTS);
    nvs_set_blob(h, "z2", g_dash_cfg.z2, Z2_SLOTS);
    nvs_set_blob(h, "z3", g_dash_cfg.z3, Z3_SLOTS);

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved");
}
