/**
 * cam_link_pump.cpp — main-side bridge from g_state to the camera link.
 *
 * Reads the laptimer's live state and pushes telemetry frames over the
 * UART link to the cam module while a recording is active. Throttled per
 * channel so we don't waste UART bandwidth (~1 kB/s budget at 115200).
 *
 * Kept separate from cam_link.cpp so that the cam-firmware repo can reuse
 * cam_link.{h,cpp} without inheriting any g_state dependency.
 */

#include "cam_link.h"
#include "../data/lap_data.h"
#include "esp_timer.h"

#include <string.h>

/* Per-channel throttle (ms between sends). GPS already arrives at ~10 Hz
 * from the receiver; OBD/CAN updates can be much faster — we cap the
 * forward rate so the link stays headroom-positive. */
#define PUMP_GPS_PERIOD_MS     100   /* 10 Hz */
#define PUMP_OBD_PERIOD_MS     50    /* 20 Hz */
#define PUMP_ANALOG_PERIOD_MS  100   /* 10 Hz */

static uint64_t s_last_gps_ms    = 0;
static uint64_t s_last_obd_ms    = 0;
static uint64_t s_last_analog_ms = 0;

/* Anchor for telemetry timestamps. We send esp_timer ms (boot-monotonic
 * on the main side) — the cam stamps its own monotonic clock too, and
 * REC_START exchanges the GPS-UTC anchor. Studio uses the GPS-UTC anchor
 * + per-frame video PTS to align telemetry with video on import. */
static inline uint64_t now_ms(void) { return esp_timer_get_time() / 1000; }

extern "C" void cam_link_pump_telemetry(void)
{
    /* Cheap exit when not recording — saves the struct copies + UART bytes. */
    if (!cam_link_is_recording()) {
        return;
    }

    uint64_t t = now_ms();

    if (t - s_last_gps_ms >= PUMP_GPS_PERIOD_MS) {
        s_last_gps_ms = t;
        const GpsData &g = g_state.gps;
        CamTelemetryGps p = {};
        p.gps_utc_ms   = t;
        p.lat          = g.lat;
        p.lon          = g.lon;
        p.speed_kmh    = g.speed_kmh;
        p.heading_deg  = g.heading_deg;
        p.altitude_m   = g.altitude_m;
        p.hdop         = g.hdop;
        p.satellites   = g.satellites;
        p.valid        = g.valid ? 1 : 0;
        cam_link_send_gps(&p);
    }

    if (t - s_last_obd_ms >= PUMP_OBD_PERIOD_MS) {
        s_last_obd_ms = t;
        const ObdData &o = g_state.obd;
        CamTelemetryObd p = {};
        p.gps_utc_ms     = t;
        p.rpm            = o.rpm;
        p.throttle_pct   = o.throttle_pct;
        p.boost_kpa      = o.boost_kpa;
        p.lambda         = o.lambda;
        p.brake_pct      = o.brake_pct;
        p.steering_angle = o.steering_angle;
        p.coolant_temp_c = o.coolant_temp_c;
        p.intake_temp_c  = o.intake_temp_c;
        p.connected      = o.connected ? 1 : 0;
        cam_link_send_obd(&p);
    }

    if (t - s_last_analog_ms >= PUMP_ANALOG_PERIOD_MS) {
        s_last_analog_ms = t;
        CamTelemetryAnalog p = {};
        p.gps_utc_ms = t;
        uint8_t mask = 0;
        for (int i = 0; i < ANALOG_CHANNELS && i < 4; i++) {
            const AnalogChannel &a = g_state.analog[i];
            p.raw_mv[i] = a.raw_mv;
            p.value[i]  = a.value;
            if (a.valid) mask |= (uint8_t)(1u << i);
        }
        p.valid_mask = mask;
        cam_link_send_analog(&p);
    }
}

/* Build + send a lap marker from a freshly-completed RecordedLap. Called
 * from lap_timer's finish_lap() right after session_store_save_lap(). */
extern "C" void cam_link_send_lap_marker_for(uint8_t lap_idx)
{
    if (!cam_link_is_recording()) return;
    if (lap_idx >= g_state.session.lap_count) return;

    const RecordedLap &rl = g_state.session.laps[lap_idx];
    CamLapMarker m = {};
    m.gps_utc_ms   = now_ms();
    m.lap_no       = (uint16_t)(lap_idx + 1);
    m.lap_ms       = rl.total_ms;
    m.sectors_used = rl.sectors_used;
    m.is_best_lap  = (lap_idx == g_state.session.best_lap_idx) ? 1 : 0;
    for (uint8_t i = 0; i < rl.sectors_used && i < 8; i++) {
        m.sector_ms[i] = rl.sector_ms[i];
    }
    cam_link_send_lap_marker(&m);
}
