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
#include "gps/gps.h"

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
void setup()
{
  Serial.begin(115200);

  gfx.begin();

  lv_init();
  lv_tick_set_cb(my_tick_function);

  // Allocate draw buffers in internal RAM for best DMA performance
  disp_draw_buf  = (lv_color_t *)heap_caps_malloc(buf_size_in_bytes,
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  disp_draw_buf2 = (lv_color_t *)heap_caps_malloc(buf_size_in_bytes,
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

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

  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  // GPS initialisieren (Tau1201 auf UART1, GPIO 16/15)
  gps_init();

  // Build the UI (Splash → Haupt-UI)
  lv_my_setup();

  Serial.println("Setup complete");
}

void loop()
{
  gps_poll();          // Tau1201 NMEA-Bytes lesen & parsen (nicht blockierend)
  lv_timer_handler();
  delay(5);
}
