/**
 * BRL Laptimer Camera Module — main.c
 *
 * Hardware: DFRobot FireBeetle 2 ESP32-P4 (DFR1172) +
 *           Raspberry Pi Camera v1.3 (OV5647) on MIPI-CSI +
 *           microSD (built-in SDIO slot) +
 *           ESP32-C6 wireless co-processor (onboard, esp_hosted).
 *
 * Responsibilities:
 *   1. Capture MIPI-CSI video → MJPEG → /sdcard/sessions/<id>/video.avi
 *      (handled by recorder.c — currently STUB; see TODOs there).
 *   2. Mirror the laptimer's GPS / OBD / analog telemetry to the SD as
 *      a .brl-format file alongside the video, so the offline workflow
 *      (Cam-SD → Laptop → Studio) has all the data it needs without
 *      ever touching the laptimer.
 *   3. Join the laptimer's WiFi AP and serve videos via HTTP. The
 *      laptimer's data_server hands phones a 302 redirect to us; we
 *      stream the AVI directly so the laptimer's WiFi stack stays free.
 *   4. Send a STATUS frame back to the laptimer every 500 ms so its
 *      UI knows whether REC is actually running and what our IP is.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cam_link/cam_link_uart.h"
#include "recorder/recorder.h"
#include "wifi_sta/wifi_sta.h"
#include "http_server/http_server.h"

static const char *TAG = "brl-cam";

#define STATUS_PERIOD_MS  500

/* Forward — defined below, called from cam_link_uart on every valid frame. */
void cam_link_on_frame(uint8_t type, const uint8_t *payload, uint16_t len);

/* ── Periodic STATUS heartbeat ───────────────────────────────────────── */
static void send_status_now(void)
{
    CamStatusPayload s = {};
    s.rec_active   = recorder_is_active() ? 1 : 0;
    s.sd_free_pct  = recorder_get_sd_free_pct();
    s.cam_connected= recorder_has_sensor() ? 1 : 0;
    s.wifi_sta_up  = wifi_sta_is_up() ? 1 : 0;
    s.err_code     = CAM_ERR_OK;
    s.cur_session_bytes = recorder_get_session_bytes();
    s.uptime_ms    = (uint64_t)(esp_timer_get_time() / 1000);
    s.http_port    = http_server_port();
    strncpy(s.ip_addr, wifi_sta_get_ip(), sizeof(s.ip_addr) - 1);
    cam_link_send_status(&s);
}

/* ── cam_link inbound frame dispatcher ───────────────────────────────── */
void cam_link_on_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    switch (type) {
    case CAM_FRAME_REC_START: {
        if (len != sizeof(CamRecStartPayload)) {
            cam_link_send_nack(type, CAM_ERR_PROTO_MISMATCH);
            break;
        }
        const CamRecStartPayload *p = (const CamRecStartPayload *)payload;
        if (p->proto_ver != CAM_LINK_PROTO_VER) {
            cam_link_send_nack(type, CAM_ERR_PROTO_MISMATCH);
            break;
        }
        RecorderSessionInfo info = {};
        memcpy(info.session_id, p->session_id, sizeof(info.session_id));
        info.gps_utc_ms = p->gps_utc_ms;
        info.track_idx  = p->track_idx;
        memcpy(info.track_name, p->track_name, sizeof(info.track_name));
        memcpy(info.car_name,   p->car_name,   sizeof(info.car_name));
        if (recorder_start(&info)) cam_link_send_ack(type);
        else                        cam_link_send_nack(type, CAM_ERR_NO_SD);
        send_status_now();   /* immediate STATUS so the UI updates */
        break;
    }
    case CAM_FRAME_REC_STOP:
        recorder_stop();
        cam_link_send_ack(type);
        send_status_now();
        break;

    case CAM_FRAME_PING:
        cam_link_send_ack(type);
        break;

    case CAM_FRAME_TELE_GPS:
        if (len == sizeof(CamTelemetryGps))
            recorder_on_telemetry_gps((const CamTelemetryGps *)payload);
        break;
    case CAM_FRAME_TELE_OBD:
        if (len == sizeof(CamTelemetryObd))
            recorder_on_telemetry_obd((const CamTelemetryObd *)payload);
        break;
    case CAM_FRAME_TELE_ANALOG:
        if (len == sizeof(CamTelemetryAnalog))
            recorder_on_telemetry_analog((const CamTelemetryAnalog *)payload);
        break;
    case CAM_FRAME_LAP_MARKER:
        if (len == sizeof(CamLapMarker))
            recorder_on_lap_marker((const CamLapMarker *)payload);
        break;

    default:
        ESP_LOGW(TAG, "unhandled frame type 0x%02X len=%u", type, len);
        break;
    }
}

/* ── Logic task (Core 0) ─────────────────────────────────────────────── */
static void logic_task(void *param)
{
    (void)param;
    uint32_t last_status_ms = 0;
    for (;;) {
        cam_link_uart_poll();
        recorder_poll();

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_status_ms >= STATUS_PERIOD_MS) {
            last_status_ms = now;
            send_status_now();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  BRL LAPTIMER CAMERA — boot");
    ESP_LOGI(TAG, "  Build: " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    cam_link_uart_init();
    recorder_init();
    wifi_sta_init();
    http_server_start();

    xTaskCreatePinnedToCore(logic_task, "logic",
                            8192, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Setup complete — waiting for laptimer commands");
}
