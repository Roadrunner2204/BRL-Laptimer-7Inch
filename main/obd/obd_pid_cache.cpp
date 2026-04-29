/**
 * obd_pid_cache.cpp — NVS-backed per-vehicle PID availability memory.
 * See obd_pid_cache.h for the storage schema.
 */

#include "obd_pid_cache.h"
#include "../compat.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "obd_pidc";
static const char *NVS_NS = "obd_pidc";

#define PID_TABLE_SZ 256

static uint8_t s_table[PID_TABLE_SZ] = {};
static char    s_key[16] = "default";   // NVS keys: ≤15 chars + NUL
static bool    s_loaded  = false;
static bool    s_dirty   = false;

static void make_nvs_key(const char *car_name, char *out, size_t out_len)
{
    // NVS key max 15 chars. We hash long names by truncating + replacing
    // path-unsafe chars; collisions are unlikely between profiles a user
    // actively curates, and a collision just means a re-learn — no crash.
    if (!car_name || car_name[0] == '\0') {
        strncpy(out, "default", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    size_t i = 0;
    for (; car_name[i] && i < out_len - 1; i++) {
        char c = car_name[i];
        if (c == ' ' || c == '/' || c == '\\' || c == '.' || c == ':')
            c = '_';
        out[i] = c;
    }
    out[i] = '\0';
}

void obd_pid_cache_load(const char *car_profile_name)
{
    char new_key[16];
    make_nvs_key(car_profile_name, new_key, sizeof(new_key));

    // Same car as last load? Skip the NVS round-trip.
    if (s_loaded && strcmp(new_key, s_key) == 0) return;

    // Persist outgoing cache before switching keys
    if (s_loaded && s_dirty) {
        obd_pid_cache_save_if_dirty();
    }

    strncpy(s_key, new_key, sizeof(s_key) - 1);
    s_key[sizeof(s_key) - 1] = '\0';
    memset(s_table, PID_UNKNOWN, sizeof(s_table));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "load(%s): no namespace yet — starting fresh",
                 s_key);
        s_loaded = true;
        s_dirty  = false;
        return;
    }
    size_t len = sizeof(s_table);
    err = nvs_get_blob(h, s_key, s_table, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(s_table)) {
        int alive = 0, dead = 0;
        for (size_t i = 0; i < sizeof(s_table); i++) {
            if (s_table[i] == PID_ALIVE) alive++;
            else if (s_table[i] == PID_DEAD) dead++;
        }
        ESP_LOGI(TAG, "load(%s): %d alive, %d dead PIDs from NVS",
                 s_key, alive, dead);
    } else {
        memset(s_table, PID_UNKNOWN, sizeof(s_table));
        ESP_LOGI(TAG, "load(%s): no entry — starting fresh", s_key);
    }
    s_loaded = true;
    s_dirty  = false;
}

PidCacheStatus obd_pid_cache_get(uint8_t pid)
{
    if (!s_loaded) return PID_UNKNOWN;
    return (PidCacheStatus)s_table[pid];
}

void obd_pid_cache_set(uint8_t pid, PidCacheStatus status)
{
    if (!s_loaded) return;
    if (s_table[pid] == (uint8_t)status) return;
    s_table[pid] = (uint8_t)status;
    s_dirty = true;
}

bool obd_pid_cache_is_dead(uint8_t pid)
{
    if (!s_loaded) return false;
    return s_table[pid] == PID_DEAD;
}

void obd_pid_cache_save_if_dirty(void)
{
    if (!s_loaded || !s_dirty) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, s_key, s_table, sizeof(s_table));
    if (err == ESP_OK) {
        nvs_commit(h);
        s_dirty = false;
        int alive = 0, dead = 0;
        for (size_t i = 0; i < sizeof(s_table); i++) {
            if (s_table[i] == PID_ALIVE) alive++;
            else if (s_table[i] == PID_DEAD) dead++;
        }
        ESP_LOGI(TAG, "save(%s): %d alive, %d dead", s_key, alive, dead);
    } else {
        ESP_LOGW(TAG, "save: nvs_set_blob failed: %s",
                 esp_err_to_name(err));
    }
    nvs_close(h);
}

void obd_pid_cache_clear(void)
{
    memset(s_table, PID_UNKNOWN, sizeof(s_table));
    s_dirty = true;
    obd_pid_cache_save_if_dirty();
    ESP_LOGI(TAG, "cleared(%s)", s_key);
}

void obd_pid_cache_counts(int *alive_out, int *dead_out)
{
    int alive = 0, dead = 0;
    if (s_loaded) {
        for (size_t i = 0; i < sizeof(s_table); i++) {
            if (s_table[i] == PID_ALIVE) alive++;
            else if (s_table[i] == PID_DEAD) dead++;
        }
    }
    if (alive_out) *alive_out = alive;
    if (dead_out)  *dead_out  = dead;
}
