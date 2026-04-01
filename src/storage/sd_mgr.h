#pragma once
#include <stdbool.h>
#include <stddef.h>

/**
 * sd_mgr — SD card manager (SPI)
 *
 * Pin mapping (free GPIOs on Waveshare 7" board):
 *   SD_MOSI = GPIO 11
 *   SD_MISO = GPIO 13  (Note: GPS_PPS moved to GPIO 16 to free 13)
 *   SD_SCLK = GPIO 12
 *   SD_CS   = GPIO 15
 *
 * Creates /sessions directory on first use.
 * Uses Arduino SD library (included with espressif32).
 */

#define SD_MOSI_PIN  11
#define SD_MISO_PIN  13
#define SD_SCLK_PIN  12
#define SD_CS_PIN    15

bool  sd_mgr_init();                              // returns true if card found
bool  sd_mgr_available();

bool  sd_write_file(const char *path, const char *data, size_t len);
bool  sd_append_file(const char *path, const char *data, size_t len);
bool  sd_read_file(const char *path, char *buf, size_t buf_size);
bool  sd_file_exists(const char *path);
bool  sd_delete_file(const char *path);
bool  sd_make_dir(const char *path);
