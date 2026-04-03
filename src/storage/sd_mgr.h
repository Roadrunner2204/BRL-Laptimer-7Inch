#pragma once
#include <stdbool.h>
#include <stddef.h>

/**
 * sd_mgr — SD card manager (SPI)
 *
 * Pin mapping (Waveshare ESP32-S3-Touch-LCD-7"):
 *   SD_MOSI = GPIO 11
 *   SD_MISO = GPIO 13
 *   SD_SCLK = GPIO 12
 *   SD_CS   = GPIO  4   ← from Waveshare example library
 *
 * Note: GPIO 4 is also the touch INT line in our Pins.h.
 * TOUCH_INT is set to -1 (polling mode) to free GPIO 4 for SD_CS.
 * Touch still works — LVGL polls via I2C on every frame.
 *
 * Creates /sessions and /tracks directories on first boot.
 */

#define SD_MOSI_PIN  11
#define SD_MISO_PIN  13
#define SD_SCLK_PIN  12
#define SD_CS_PIN     4

bool  sd_mgr_init();                              // returns true if card found
bool  sd_mgr_available();

bool  sd_write_file(const char *path, const char *data, size_t len);
bool  sd_append_file(const char *path, const char *data, size_t len);
bool  sd_read_file(const char *path, char *buf, size_t buf_size);
bool  sd_file_exists(const char *path);
bool  sd_delete_file(const char *path);
bool  sd_make_dir(const char *path);
