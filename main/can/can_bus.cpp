/**
 * can_bus.cpp -- Direct CAN bus reader via TWAI + on-board TJA1051
 *
 * Reads raw CAN frames, matches them against the active car profile's
 * sensor definitions, extracts values using scale/offset, and writes
 * them into g_state.obd.
 */

#include "can_bus.h"
#include "../data/lap_data.h"
#include "../data/car_profile.h"
#include "compat.h"

#include "driver/twai.h"
#include "esp_log.h"

#include <string.h>
#include <math.h>

static const char *TAG = "can_bus";

static bool s_running = false;

// ---------------------------------------------------------------------------
// Map sensor name -> g_state.obd field
// ---------------------------------------------------------------------------
static void apply_sensor_value(const char *name, float value)
{
    if      (strcmp(name, "RPM") == 0)        g_state.obd.rpm = value;
    else if (strcmp(name, "TPS") == 0)        g_state.obd.throttle_pct = value;
    else if (strcmp(name, "Boost") == 0)      g_state.obd.boost_kpa = value;
    else if (strcmp(name, "MAP") == 0)        g_state.obd.boost_kpa = value;
    else if (strcmp(name, "Lambda") == 0)     g_state.obd.lambda = value;
    else if (strcmp(name, "WaterT") == 0)     g_state.obd.coolant_temp_c = value;
    else if (strcmp(name, "CoolantT") == 0)   g_state.obd.coolant_temp_c = value;
    else if (strcmp(name, "IntakeT") == 0)    g_state.obd.intake_temp_c = value;
    else if (strcmp(name, "IAT") == 0)        g_state.obd.intake_temp_c = value;
    else if (strcmp(name, "Brake") == 0)      g_state.obd.brake_pct = value;
    else if (strcmp(name, "Steering") == 0)   g_state.obd.steering_angle = value;
    // Other sensors are available via car_profile_find_sensor() but
    // don't have a dedicated g_state.obd field (yet).
}

// ---------------------------------------------------------------------------
// Extract a signal value from a CAN data frame using sensor definition
// ---------------------------------------------------------------------------
static float extract_value(const uint8_t *data, uint8_t dlc, const CarSensor *s)
{
    if (s->start >= dlc) return 0.0f;

    int32_t raw = 0;
    if (s->len == 2 && s->start + 1 < dlc) {
        // Big-endian (Motorola byte order, standard for most automotive CAN)
        raw = ((int32_t)data[s->start] << 8) | data[s->start + 1];
    } else {
        raw = data[s->start];
    }

    if (!s->is_unsigned && s->len == 2 && raw > 32767) {
        raw -= 65536;  // sign-extend 16-bit
    } else if (!s->is_unsigned && s->len == 1 && raw > 127) {
        raw -= 256;    // sign-extend 8-bit
    }

    float value = (float)raw * s->scale + s->offset;

    // Clamp to defined range
    if (value < s->min_val) value = s->min_val;
    if (value > s->max_val) value = s->max_val;

    return value;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool can_bus_init(void)
{
    if (s_running) {
        log_w("CAN bus already running");
        return true;
    }

    if (!g_car_profile.loaded) {
        log_e("No car profile loaded — cannot determine CAN bitrate");
        return false;
    }

    // Determine timing from car profile bitrate
    twai_timing_config_t t_config;
    switch (g_car_profile.bitrate) {
        case 125:  t_config = TWAI_TIMING_CONFIG_125KBITS();  break;
        case 250:  t_config = TWAI_TIMING_CONFIG_250KBITS();  break;
        case 500:  t_config = TWAI_TIMING_CONFIG_500KBITS();  break;
        case 1000: t_config = TWAI_TIMING_CONFIG_1MBITS();    break;
        default:
            log_e("Unsupported CAN bitrate: %d kBit/s", g_car_profile.bitrate);
            return false;
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    g_config.rx_queue_len = 32;

    // Accept all CAN IDs (filter by software using car profile)
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) {
        log_e("TWAI driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    err = twai_start();
    if (err != ESP_OK) {
        log_e("TWAI start failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return false;
    }

    s_running = true;
    g_state.obd.connected = true;
    log_i("CAN bus started: %d kBit/s on TX=GPIO%d RX=GPIO%d (listen-only)",
          g_car_profile.bitrate, CAN_TX_PIN, CAN_RX_PIN);
    return true;
}

void can_bus_poll(void)
{
    if (!s_running) return;

    // Process up to 16 frames per poll cycle to avoid starving other tasks
    for (int n = 0; n < 16; n++) {
        twai_message_t msg;
        if (twai_receive(&msg, 0) != ESP_OK) break;  // no more messages

        // Skip RTR and error frames
        if (msg.rtr || msg.dlc_non_comp) continue;

        uint32_t id = msg.identifier;

        // Match against all sensors in active car profile
        for (int i = 0; i < g_car_profile.sensor_count; i++) {
            const CarSensor *s = &g_car_profile.sensors[i];
            if (s->can_id == id && s->proto == 0) {
                float val = extract_value(msg.data, msg.data_length_code, s);
                apply_sensor_value(s->name, val);
            }
        }
    }
}

void can_bus_stop(void)
{
    if (!s_running) return;

    twai_stop();
    twai_driver_uninstall();
    s_running = false;
    g_state.obd.connected = false;
    log_i("CAN bus stopped");
}

bool can_bus_active(void)
{
    return s_running;
}
