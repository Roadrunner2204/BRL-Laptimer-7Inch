/**
 * BRL Laptimer — Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 *
 * Hardware:
 *   - ESP32-P4 dual-core RISC-V @ 360 MHz, 32 MB PSRAM
 *   - 7" 1024×600 MIPI DSI LCD (EK79007 driver)
 *   - GT911 capacitive touch controller
 *   - ESP32-C6 co-processor for Wi-Fi 6 / Bluetooth 5 (LE)
 *   - ES8311 audio codec + NS4150B amplifier
 *
 * Architecture:
 *   Core 0 — logic task: GPS · OBD · WiFi · Timing
 *   Core 1 — LVGL task:  UI rendering (managed by BSP/esp_lvgl_port)
 *
 * Libraries:
 *   - Waveshare BSP  (display + touch + audio driver)
 *   - LVGL 9         (UI framework)
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

/* Application modules */
#include "gps/gps.h"
#include "obd/obd_bt.h"
#include "wifi/wifi_mgr.h"
#include "wifi/data_server.h"
#include "storage/sd_mgr.h"
#include "storage/session_store.h"
#include "timing/lap_timer.h"
#include "timing/live_delta.h"
#include "data/lap_data.h"
#include "data/car_profile.h"
#include "can/can_bus.h"
#include "video/video_mgr.h"
#include "video/video_pipeline.h"

static const char *TAG = "brl-laptimer";

/* Cross-core mutex — protects g_state between logic and LVGL tasks */
SemaphoreHandle_t g_state_mutex = NULL;

/* Forward declaration — UI entry point (lv_code.c → app.cpp) */
extern void lv_my_setup(void);

/* ------------------------------------------------------------------
 * Core 0 task: GPS · OBD · WiFi · Timing  (non-LVGL work)
 * ------------------------------------------------------------------ */
static void logic_task(void *param)
{
    (void)param;
    for (;;) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        gps_poll();
        lap_timer_poll();
        xSemaphoreGive(g_state_mutex);

        /* OBD/CAN, WiFi and HTTP don't touch g_state directly in hot paths */
        if (g_state.veh_conn_mode == VEH_CONN_CAN_DIRECT)
            can_bus_poll();
        else
            obd_bt_poll();
        wifi_mgr_poll();
        data_server_poll();

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ------------------------------------------------------------------
 * app_main — runs on Core 0 initially, then spawns tasks
 * ------------------------------------------------------------------ */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  BRL LAPTIMER  BOOT  v2.0-P4");
    ESP_LOGI(TAG, "  Build: " __DATE__ " " __TIME__);
    ESP_LOGI(TAG, "========================================");

    /* NVS — needed by Wi-Fi and config storage */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_state_mutex = xSemaphoreCreateMutex();

    /* ── JPEG HW codec: MUST init before display BSP (DMA fragmentation) ── */
    ESP_LOGI(TAG, "video_pipeline_early_init");
    video_pipeline_early_init();

    /* ── Display via BSP (MIPI DSI + EK79007 + GT911 touch) ────── */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .sw_rotate = false,
        }
    };
    /* Pin LVGL to Core 1 so it never competes with USB/SD/GPS on Core 0.
     * Priority 4 stays above our video pipeline tasks (pri 3) so the UI
     * always wins scheduler time while recording. */
    cfg.lvgl_port_cfg.task_affinity = 1;
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    /* Dark theme */
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        bsp_display_lock(0);
        lv_theme_t *theme = lv_theme_default_init(disp,
            lv_color_hex(0x0096FF),   /* primary: BRL blue */
            lv_color_hex(0x0060C0),   /* secondary */
            true,                      /* dark mode */
            &lv_font_montserrat_14);
        lv_display_set_theme(disp, theme);
        /* Rotation disabled — requires too much RAM for rotation buffer.
         * If display is upside-down, mount the board differently. */
        bsp_display_unlock();
    }

    /* ── GPS: UART (RX=GPIO2, TX=GPIO3, PPS=GPIO4) ─────────── */
    ESP_LOGI(TAG, "gps_init");
    gps_init();

    /* ── SD card ─────────────────────────────────────────────── */
    ESP_LOGI(TAG, "sd_mgr_init");
    sd_mgr_init();
    g_state.sd_available = sd_mgr_available();

    /* ── Load tracks from SD ─────────────────────────────────── */
    ESP_LOGI(TAG, "session_store_load");
    session_store_load_user_tracks();
    session_store_load_builtin_overrides();

    /* ── Load car profile from SD (if previously selected) ─── */
    {
        char active_car[32] = {};
        car_profile_get_active(active_car, sizeof(active_car));
        if (strlen(active_car) > 0) {
            ESP_LOGI(TAG, "car_profile_load: %s", active_car);
            car_profile_load(active_car);
        }
    }

    /* ── Lap timer ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "lap_timer_init");
    lap_timer_init();

    /* ── WiFi (ESP32-C6 co-processor via SDIO / esp_hosted) ─── */
    ESP_LOGI(TAG, "wifi_mgr_init");
    wifi_mgr_init();

    /* Auto-start AP so the Android telemetry app can connect right away.
     * User can still switch to STA/OFF via the settings screen. */
    ESP_LOGI(TAG, "wifi_set_mode(BRL_WIFI_AP)");
    wifi_set_mode(BRL_WIFI_AP);

    /* ── Video recording (USB camera, JPEG pipeline, AVI) ───── */
    ESP_LOGI(TAG, "video_init");
    video_init();

    /* ── OBD / CAN vehicle data ──────────────────────────────── */
    ESP_LOGI(TAG, "obd_bt_init");
    obd_bt_init();

    /* ── Build LVGL UI (Splash → Main Menu) ──────────────────── */
    ESP_LOGI(TAG, "lv_my_setup");
    bsp_display_lock(0);
    lv_my_setup();
    bsp_display_unlock();
    ESP_LOGI(TAG, "lv_my_setup DONE");

    /* ── CAN bus auto-start (config loaded by lv_my_setup → dash_config_load) */
    if (g_state.veh_conn_mode == VEH_CONN_CAN_DIRECT && g_car_profile.loaded) {
        ESP_LOGI(TAG, "can_bus_init (auto-start)");
        can_bus_init();
    }

    /* ── Spawn logic task on Core 0 ──────────────────────────── */
    xTaskCreatePinnedToCore(
        logic_task,
        "logic",
        8192,       /* stack bytes */
        NULL,
        1,          /* priority */
        NULL,
        0           /* Core 0 */
    );

    ESP_LOGI(TAG, "Setup complete — logic on Core 0, LVGL on Core 1 (BSP)");

    /* Note: Unlike Arduino loop(), the BSP's esp_lvgl_port handles the
     * LVGL timer in its own task. app_main() can return here. The logic
     * task and LVGL task run independently via FreeRTOS.
     *
     * If g_state needs guarding inside LVGL timers, use bsp_display_lock()
     * which wraps the LVGL port mutex. */
}
