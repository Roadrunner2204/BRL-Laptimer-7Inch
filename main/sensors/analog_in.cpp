/**
 * analog_in.cpp -- 4 ADC1 inputs (GPIO 20/21/22/23 = AN1..AN4)
 */

#include "analog_in.h"
#include "../compat.h"
#include "../obd/obd_status.h"
#include "../ui/dash_config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "analog_in";

#define NVS_NAMESPACE "analog"

// ---------------------------------------------------------------------------
// Channel mapping (compile-time, fixed by the board layout)
// ---------------------------------------------------------------------------
// ESP32-P4 ADC1: channel n is on GPIO (16 + n).
// SDIO to the C6 occupies channels 0..3 (GPIO 16..19), so we use 4..7 here.
static const adc_channel_t  ADC_CHANNELS[ANALOG_CHANNELS] = {
    ADC_CHANNEL_4,  // GPIO 20  -> AN1
    ADC_CHANNEL_5,  // GPIO 21  -> AN2
    ADC_CHANNEL_6,  // GPIO 22  -> AN3
    ADC_CHANNEL_7,  // GPIO 23  -> AN4
};

// ---------------------------------------------------------------------------
// Driver handles (lazy-init on first analog_in_init call)
// ---------------------------------------------------------------------------
static adc_oneshot_unit_handle_t s_adc1 = nullptr;
static adc_cali_handle_t         s_cali[ANALOG_CHANNELS] = {};
static bool                      s_ready = false;

// ---------------------------------------------------------------------------
// User-editable configuration (defaults: passthrough mV, generic name)
// ---------------------------------------------------------------------------
AnalogChannelCfg g_analog_cfg[ANALOG_CHANNELS] = {
    { "AN1", 1.0f, 0.0f, 0.0f, 3300.0f, false },
    { "AN2", 1.0f, 0.0f, 0.0f, 3300.0f, false },
    { "AN3", 1.0f, 0.0f, 0.0f, 3300.0f, false },
    { "AN4", 1.0f, 0.0f, 0.0f, 3300.0f, false },
};

// ---------------------------------------------------------------------------
// init: ADC1 unit + per-channel oneshot config + curve-fit calibration
// ---------------------------------------------------------------------------
void analog_in_init(void)
{
    if (s_ready) return;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .clk_src  = (adc_oneshot_clk_src_t)0,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc1);
    if (err != ESP_OK) {
        log_e("adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten    = ADC_ATTEN_DB_12,    // ~0..3.1 V usable on a 3.3 V rail
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        err = adc_oneshot_config_channel(s_adc1, ADC_CHANNELS[i], &ch_cfg);
        if (err != ESP_OK) {
            log_e("adc_oneshot_config_channel(AN%d) failed: %s",
                  i + 1, esp_err_to_name(err));
            continue;
        }
        // ESP32-P4 in ESP-IDF v5.4.1 ships no ADC calibration scheme
        // (esp_adc/esp32p4/include/adc_cali_schemes.h is empty). Both
        // ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED and _LINE_FITTING_ are 0,
        // so the API symbols are not even declared. poll() already falls
        // back to (raw * 3300 / 4095) when s_cali[i] is nullptr.
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id  = ADC_UNIT_1,
            .chan     = ADC_CHANNELS[i],
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali[i]) != ESP_OK) {
            s_cali[i] = nullptr;
            log_w("AN%d: no curve-fitting cal, using raw counts", i + 1);
        }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id  = ADC_UNIT_1,
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali[i]) != ESP_OK) {
            s_cali[i] = nullptr;
            log_w("AN%d: no line-fitting cal, using raw counts", i + 1);
        }
#else
        s_cali[i] = nullptr;
#endif
    }

    s_ready = true;
    log_i("Analog inputs initialised: AN1..AN4 on GPIO 20/21/22/23 (ADC1 ch4..7)");
}

// ---------------------------------------------------------------------------
// NVS load / save  (per-channel: name, scale, offset, min, max, enabled)
// ---------------------------------------------------------------------------
static void key_for(int idx, const char *suffix, char *out, size_t out_len)
{
    // NVS keys are limited to 15 chars; "AN1_scale" etc. fits well.
    snprintf(out, out_len, "AN%d_%s", idx + 1, suffix);
}

void analog_in_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        log_i("No saved analog config — using defaults");
        return;
    }
    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        char key[16];
        size_t name_len = sizeof(g_analog_cfg[i].name);

        key_for(i, "name", key, sizeof(key));
        nvs_get_str(h, key, g_analog_cfg[i].name, &name_len);

        key_for(i, "scale", key, sizeof(key));
        size_t blob_len = sizeof(float);
        nvs_get_blob(h, key, &g_analog_cfg[i].scale, &blob_len);

        key_for(i, "ofs", key, sizeof(key));
        blob_len = sizeof(float);
        nvs_get_blob(h, key, &g_analog_cfg[i].offset, &blob_len);

        key_for(i, "min", key, sizeof(key));
        blob_len = sizeof(float);
        nvs_get_blob(h, key, &g_analog_cfg[i].min_val, &blob_len);

        key_for(i, "max", key, sizeof(key));
        blob_len = sizeof(float);
        nvs_get_blob(h, key, &g_analog_cfg[i].max_val, &blob_len);

        uint8_t en = g_analog_cfg[i].enabled ? 1 : 0;
        key_for(i, "en", key, sizeof(key));
        nvs_get_u8(h, key, &en);
        g_analog_cfg[i].enabled = (en != 0);
    }
    nvs_close(h);
    log_i("Analog config loaded");
}

void analog_in_save_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        log_e("nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }
    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        char key[16];
        key_for(i, "name", key, sizeof(key));
        nvs_set_str(h, key, g_analog_cfg[i].name);

        key_for(i, "scale", key, sizeof(key));
        nvs_set_blob(h, key, &g_analog_cfg[i].scale, sizeof(float));

        key_for(i, "ofs", key, sizeof(key));
        nvs_set_blob(h, key, &g_analog_cfg[i].offset, sizeof(float));

        key_for(i, "min", key, sizeof(key));
        nvs_set_blob(h, key, &g_analog_cfg[i].min_val, sizeof(float));

        key_for(i, "max", key, sizeof(key));
        nvs_set_blob(h, key, &g_analog_cfg[i].max_val, sizeof(float));

        key_for(i, "en", key, sizeof(key));
        nvs_set_u8(h, key, g_analog_cfg[i].enabled ? 1 : 0);
    }
    nvs_commit(h);
    nvs_close(h);
    log_i("Analog config saved");
}

// ---------------------------------------------------------------------------
// Polling: one read per enabled channel; disabled channels are zeroed so
// the UI shows '---' instead of the previous stale value.
// ---------------------------------------------------------------------------
void analog_in_poll(void)
{
    if (!s_ready) return;

    for (int i = 0; i < ANALOG_CHANNELS; i++) {
        AnalogChannel    &out = g_state.analog[i];
        AnalogChannelCfg &cfg = g_analog_cfg[i];

        if (!cfg.enabled) {
            out.raw_mv = 0;
            out.value  = 0.0f;
            out.valid  = false;
            continue;
        }

        int raw = 0;
        if (adc_oneshot_read(s_adc1, ADC_CHANNELS[i], &raw) != ESP_OK) {
            out.valid = false;
            continue;
        }

        int mv = 0;
        if (s_cali[i] && adc_cali_raw_to_voltage(s_cali[i], raw, &mv) == ESP_OK) {
            out.raw_mv = mv;
        } else {
            // No cal scheme — fall back to a linear assumption
            // (raw / 4095 * 3300). Fine for relative readings.
            out.raw_mv = (raw * 3300) / 4095;
        }

        float v = (float)out.raw_mv * cfg.scale + cfg.offset;
        if (v < cfg.min_val) v = cfg.min_val;
        if (v > cfg.max_val) v = cfg.max_val;
        out.value = v;
        out.valid = true;
        // Mark this analog channel "live" so the slot pickers (Display,
        // App, Studio) can grey out unconnected/disabled channels.
        obd_status_mark_active((uint8_t)(FIELD_AN1 + i));
    }
}
