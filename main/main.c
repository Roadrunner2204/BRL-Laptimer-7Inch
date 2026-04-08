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

        /* OBD, WiFi and HTTP don't touch g_state directly in hot paths */
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

    /* ── Display via BSP (MIPI DSI + EK79007 + GT911 touch) ────── */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .sw_rotate = true,
        }
    };
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
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
        bsp_display_unlock();
    }

    /* ── GPS: UART (RX=GPIO19, TX=GPIO20) ────────────────────── */
    ESP_LOGI(TAG, "gps_init");
    gps_init();

    /* ── SD card ─────────────────────────────────────────────── */
    ESP_LOGI(TAG, "sd_mgr_init");
    sd_mgr_init();

    /* ── Load tracks from SD ─────────────────────────────────── */
    ESP_LOGI(TAG, "session_store_load");
    session_store_load_user_tracks();
    session_store_load_builtin_overrides();

    /* ── Lap timer ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "lap_timer_init");
    lap_timer_init();

    /* ── WiFi (ESP32-C6 co-processor via SDIO / esp_hosted) ─── */
    ESP_LOGI(TAG, "wifi_mgr_init");
    wifi_mgr_init();

    /* ── OBD Bluetooth LE ────────────────────────────────────── */
    ESP_LOGI(TAG, "obd_bt_init");
    obd_bt_init();

    /* ── Build LVGL UI (Splash → Main Menu) ──────────────────── */
    ESP_LOGI(TAG, "lv_my_setup");
    bsp_display_lock(0);
    lv_my_setup();
    bsp_display_unlock();
    ESP_LOGI(TAG, "lv_my_setup DONE");

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
