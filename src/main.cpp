/**
 * BRL Laptimer - Waveshare ESP32-S3 7-Inch LCD
 *
 * Hardware:
 *   - Waveshare ESP32-S3 7-Inch Capacitive Touch Display
 *   - 800x480 RGB LCD, GT911 touch controller
 *   - CH422G I/O Expander for backlight / reset control
 *
 * Libraries:
 *   - LovyanGFX  (display + touch driver)
 *   - LVGL 9     (UI framework)
 */

#include <Arduino.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "gps/gps.h"
#include "obd/obd_bt.h"
#include "wifi/wifi_mgr.h"
#include "wifi/data_server.h"
#include "storage/sd_mgr.h"
#include "storage/session_store.h"
#include "timing/lap_timer.h"
#include "timing/live_delta.h"
#include "data/lap_data.h"

// ---------------------------------------------------------------------------
// Cross-core mutex — taken for any read/write of g_state from either task.
// Use LVGL_LOCK / LVGL_UNLOCK around all lv_* calls made outside the LVGL task
// (currently we never do that — LVGL stays on Core 1 entirely).
// ---------------------------------------------------------------------------
SemaphoreHandle_t g_state_mutex = nullptr;

// ---------------------------------------------------------------------------
// LovyanGFX display configuration
// ---------------------------------------------------------------------------
#include <driver/i2c.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

#define TFT_HOR_RES   800
#define TFT_VER_RES   480

#define TOUCH_SDA  8
#define TOUCH_SCL  9
#define TOUCH_INT  4
#define TOUCH_RST -1

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB   _bus_instance;
  lgfx::Panel_RGB _panel_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    {
      auto cfg = _panel_instance.config();
      cfg.memory_width  = TFT_HOR_RES;
      cfg.memory_height = TFT_VER_RES;
      cfg.panel_width   = TFT_HOR_RES;
      cfg.panel_height  = TFT_VER_RES;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }

    {
      auto cfg = _bus_instance.config();
      cfg.panel = &_panel_instance;

      // RGB data pins (B0-B4, G0-G5, R0-R4)
      cfg.pin_d0  = 14;  // B0
      cfg.pin_d1  = 38;  // B1
      cfg.pin_d2  = 18;  // B2
      cfg.pin_d3  = 17;  // B3
      cfg.pin_d4  = 10;  // B4

      cfg.pin_d5  = 39;  // G0
      cfg.pin_d6  =  0;  // G1
      cfg.pin_d7  = 45;  // G2
      cfg.pin_d8  = 48;  // G3
      cfg.pin_d9  = 47;  // G4
      cfg.pin_d10 = 21;  // G5

      cfg.pin_d11 =  1;  // R0
      cfg.pin_d12 =  2;  // R1
      cfg.pin_d13 = 42;  // R2
      cfg.pin_d14 = 41;  // R3
      cfg.pin_d15 = 40;  // R4

      cfg.pin_henable = 5;
      cfg.pin_vsync   = 3;
      cfg.pin_hsync   = 46;
      cfg.pin_pclk    = 7;
      cfg.freq_write  = 14000000;

      cfg.hsync_polarity   = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch  = 8;

      cfg.vsync_polarity   = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch  = 8;

      cfg.pclk_active_neg = 1;
      cfg.de_idle_high    = 0;
      cfg.pclk_idle_high  = 0;

      _bus_instance.config(cfg);
    }
    _panel_instance.setBus(&_bus_instance);

    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = TFT_HOR_RES - 1;
      cfg.y_min = 0;
      cfg.y_max = TFT_VER_RES - 1;
      cfg.pin_int  = TOUCH_INT;
      cfg.pin_rst  = TOUCH_RST;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = I2C_NUM_1;
      cfg.pin_sda  = TOUCH_SDA;
      cfg.pin_scl  = TOUCH_SCL;
      cfg.freq     = 400000;
      cfg.i2c_addr = 0x14;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};

static LGFX gfx;

// ---------------------------------------------------------------------------
// Forward declaration of UI setup function (defined in lv_code.cpp)
// ---------------------------------------------------------------------------
void lv_my_setup();

// ---------------------------------------------------------------------------
// LVGL helpers
// ---------------------------------------------------------------------------
static uint32_t my_tick_function(void) {
  return millis();
}

static const uint32_t screenWidth  = TFT_HOR_RES;
static const uint32_t screenHeight = TFT_VER_RES;

static const int buf_size_in_bytes =
    screenWidth * screenHeight * sizeof(lv_color_t) / 10;

static lv_color_t *disp_draw_buf  = nullptr;
static lv_color_t *disp_draw_buf2 = nullptr;

static lv_display_t *disp  = nullptr;
static lv_indev_t   *indev = nullptr;

void my_disp_flush(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  lv_draw_sw_rgb565_swap(px_map, w * h);
  gfx.pushImage(area->x1, area->y1, w, h, (uint16_t *)px_map);

  lv_disp_flush_ready(d);
}

void my_touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
  uint16_t touchX, touchY;

  if (gfx.getTouch(&touchX, &touchY)) {
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = touchX;
    data->point.y = touchY;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ---------------------------------------------------------------------------
// Arduino setup / loop
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Core 0 task: GPS · OBD · WiFi · Timing  (non-LVGL work)
// ---------------------------------------------------------------------------
static void logic_task(void * /*param*/)
{
  for (;;) {
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    gps_poll();
    lap_timer_poll();
    xSemaphoreGive(g_state_mutex);

    // OBD, WiFi and HTTP don't touch g_state directly in hot paths
    obd_bt_poll();
    wifi_mgr_poll();
    data_server_poll();

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------------------------------------------------------
// Arduino setup / loop  — run on Core 1 (APP_CPU)
// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);

  g_state_mutex = xSemaphoreCreateMutex();

  gfx.begin();

  // CH422G: set IO output mode and assert SD_CS (IO3) LOW.
  // Must run AFTER gfx.begin() so LovyanGFX has installed I2C_NUM_1.
  // CH422G multi-address protocol (from datasheet):
  //   WR_SET=0x24: config  — bit0=IO_OE(1=output)  bit2=OD_EN  bit3=SLEEP
  //   WR_IO =0x38: output data for IO0-IO7
  // Waveshare pin map: IO0=TP_RST IO1=LCD_BL IO2=LCD_RST IO3=SD_CS IO4=USB_SEL
  {
    auto i2c_write = [](uint8_t addr, uint8_t val) -> esp_err_t {
      i2c_cmd_handle_t c = i2c_cmd_link_create();
      i2c_master_start(c);
      i2c_master_write_byte(c, (addr << 1) | I2C_MASTER_WRITE, true);
      i2c_master_write_byte(c, val, true);
      i2c_master_stop(c);
      esp_err_t e = i2c_master_cmd_begin(I2C_NUM_1, c, pdMS_TO_TICKS(50));
      i2c_cmd_link_delete(c);
      return e;
    };
    // Write output VALUES first (latch set while still in input mode = no glitch)
    // THEN enable output mode — pins immediately go to correct values.
    // Prevents TP_RST/LCD_RST from briefly pulsing LOW during mode switch.
    // WR_IO=0xFF: all pins HIGH (IO0=TP_RST, IO1=LCD_BL, IO2=LCD_RST all un-reset)
    // IO3=HIGH keeps SD card D3 line HIGH → SD bus mode (not SPI mode)
    // Must latch output values BEFORE enabling output mode (prevents reset glitch)
    esp_err_t e1 = i2c_write(0x38, 0xFF); // WR_IO:  all HIGH, incl. IO3(SD D3)=HIGH
    esp_err_t e2 = i2c_write(0x24, 0x01); // WR_SET: IO_OE=1 (enable output)
    log_e("[CH422G] WR_IO->0xFF:%s  WR_SET->0x01:%s",
          e1 == ESP_OK ? "OK" : esp_err_to_name(e1),
          e2 == ESP_OK ? "OK" : esp_err_to_name(e2));
  }

  lv_init();
  lv_tick_set_cb(my_tick_function);

  // Draw buffers in PSRAM — each is 800×480×2÷10 = 76 800 B.
  // Keeping them in DRAM (MALLOC_CAP_INTERNAL) would consume ~150 KB of the
  // ~300 KB total DRAM budget, leaving too little for WiFi+NimBLE coexistence
  // (esp_timer_create inside hostap_auth_open fails → abort on client connect).
  // On ESP32-S3 with OPI PSRAM the RGB panel copies via CPU/DMA2D — PSRAM
  // source is fine; Panel_RGB pushImage() is not a DMA-from-IRAM path.
  disp_draw_buf  = (lv_color_t *)heap_caps_malloc(buf_size_in_bytes, MALLOC_CAP_SPIRAM);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(buf_size_in_bytes, MALLOC_CAP_SPIRAM);
  // Fallback to DRAM if PSRAM unavailable
  if (!disp_draw_buf)
      disp_draw_buf  = (lv_color_t *)heap_caps_malloc(buf_size_in_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!disp_draw_buf2)
      disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(buf_size_in_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  log_e("[HEAP] after LVGL buf alloc — DRAM free: %u  largest: %u",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  if (!disp_draw_buf) {
    Serial.println("LVGL: draw buffer allocation failed!");
    while (1);
  }
  if (!disp_draw_buf2) {
    Serial.println("LVGL: second draw buffer allocation failed, using single buffer");
  }

  disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, disp_draw_buf, disp_draw_buf2,
                         buf_size_in_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  // Dark theme: removes default borders/shadows from all LVGL widgets
  lv_theme_t *theme = lv_theme_default_init(disp,
      lv_color_hex(0x0096FF),   // primary: BRL blue
      lv_color_hex(0x0060C0),   // secondary
      true,                      // dark mode
      &lv_font_montserrat_14);
  lv_display_set_theme(disp, theme);

  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  // GPS: Tau1201 on UART2 (GPIO 19 RX, GPIO 20 TX)
  log_e("[SETUP] gps_init");
  gps_init();

  // SD card (SPI: MOSI=11, MISO=13, SCLK=12, CS=15)
  log_e("[SETUP] sd_mgr_init");
  sd_mgr_init();

  // Load user-created tracks from SD
  log_e("[SETUP] session_store_load");
  session_store_load_user_tracks();
  session_store_load_builtin_overrides();

  // Lap timer
  log_e("[SETUP] lap_timer_init");
  lap_timer_init();

  // WiFi manager: MUST come before obd_bt_init() — forces esp_wifi_init()
  // while heap is clean. NimBLE/BLE init reduces WiFi coex static-RX-buffer
  // count to 1 (< required 4), so WiFi init after BLE fails with ESP_ERR_NO_MEM.
  log_e("[SETUP] wifi_mgr_init");
  wifi_mgr_init();
  log_e("[HEAP] after wifi_mgr_init — DRAM free: %u  largest: %u",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  // OBD Bluetooth BLE (after WiFi so coex init order is correct)
  log_e("[SETUP] obd_bt_init");
  obd_bt_init();
  log_e("[HEAP] after obd_bt_init  — DRAM free: %u  largest: %u",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  // Build the UI (Splash → Haupt-UI)
  log_e("[SETUP] lv_my_setup");
  lv_my_setup();
  log_e("[SETUP] lv_my_setup DONE");

  // I2C scan + CH422G diagnostic — runs on UART0 (same as ESP-IDF logs)
  log_e("[I2C scan on I2C_NUM_1]");
  for (uint8_t addr = 1; addr < 127; addr++) {
    i2c_cmd_handle_t sc = i2c_cmd_link_create();
    i2c_master_start(sc);
    i2c_master_write_byte(sc, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(sc);
    if (i2c_master_cmd_begin(I2C_NUM_1, sc, pdMS_TO_TICKS(10)) == ESP_OK)
      log_e("  I2C found: 0x%02X", addr);
    i2c_cmd_link_delete(sc);
  }
  log_e("[I2C scan done]");

  // Spawn logic task pinned to Core 0 (PRO_CPU)
  // Stack: 8 kB is sufficient for GPS/OBD/WiFi poll loops
  xTaskCreatePinnedToCore(
    logic_task,
    "logic",
    8192,       // stack bytes
    nullptr,
    1,          // priority (same as loop task — yields via vTaskDelay)
    nullptr,
    0           // Core 0
  );

  Serial.println("Setup complete — logic task on Core 0, LVGL on Core 1");
}

// loop() stays on Core 1 — only drives LVGL rendering
void loop()
{
  // Guard g_state reads inside lv_timer_handler (timer_live_update)
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  lv_timer_handler();
  xSemaphoreGive(g_state_mutex);
  delay(5);
}
