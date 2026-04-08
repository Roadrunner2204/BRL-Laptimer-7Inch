/**
 * BRL Laptimer - Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 *
 * Hardware:
 *   - ESP32-P4 dual-core RISC-V @ 360 MHz, 32 MB PSRAM
 *   - 7" 1024x600 MIPI DSI LCD (EK79007 driver)
 *   - GT911 capacitive touch controller
 *   - ESP32-C6 co-processor for Wi-Fi 6 / Bluetooth 5 (LE)
 *   - ES8311 audio codec + NS4150B amplifier
 *
 * Libraries:
 *   - Waveshare BSP  (display + touch + audio driver)
 *   - LVGL 9         (UI framework)
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "brl-laptimer";

/* Forward declaration of UI setup (defined in lv_code.c) */
extern void lv_my_setup(void);

void app_main(void)
{
    /* Initialise NVS — needed by Wi-Fi and other components */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialise display via BSP (MIPI DSI + EK79007 + GT911 touch) */
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

    /* Optional: rotate display 180 degrees if mounted upside-down */
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        bsp_display_lock(0);
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
        bsp_display_unlock();
    }

    /* Build the UI (thread-safe) */
    bsp_display_lock(0);
    lv_my_setup();
    bsp_display_unlock();

    ESP_LOGI(TAG, "BRL Laptimer started — 1024x600 MIPI DSI on ESP32-P4");
}
