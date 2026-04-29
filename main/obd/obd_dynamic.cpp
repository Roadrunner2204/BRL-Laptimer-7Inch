/**
 * obd_dynamic.cpp — see obd_dynamic.h.
 *
 * Two parallel 256-entry arrays:
 *   s_value[pid]    most recent post-scale/offset float
 *   s_seen_ms[pid]  millis() timestamp of the most recent write
 *                   (0 means never written this boot)
 *
 * Lock-free single-writer (NimBLE host task hits set() from the OBD
 * decoder; LVGL task reads). uint32_t writes are atomic on RV32 so a
 * reader sees either old or new — never half-torn.
 */

#include "obd_dynamic.h"
#include "../compat.h"

#include <string.h>
#include <stdint.h>
#include <math.h>

static float    s_value[OBD_DYNAMIC_PID_COUNT]   = {};
static uint32_t s_seen_ms[OBD_DYNAMIC_PID_COUNT] = {};

void obd_dynamic_set(uint8_t pid, float value)
{
    s_value[pid]   = value;
    s_seen_ms[pid] = millis();
}

bool obd_dynamic_get(uint8_t pid, float *out_value)
{
    if (s_seen_ms[pid] == 0) return false;
    if (out_value) *out_value = s_value[pid];
    return true;
}

uint32_t obd_dynamic_age_ms(uint8_t pid)
{
    if (s_seen_ms[pid] == 0) return UINT32_MAX;
    uint32_t now = millis();
    return now - s_seen_ms[pid];
}

bool obd_dynamic_is_live(uint8_t pid)
{
    return obd_dynamic_age_ms(pid) <= OBD_DYNAMIC_LIVE_WINDOW_MS;
}

void obd_dynamic_clear(void)
{
    memset(s_value,   0, sizeof(s_value));
    memset(s_seen_ms, 0, sizeof(s_seen_ms));
}
