#pragma once
#include <stdbool.h>
#include <stddef.h>

/**
 * sd_mgr — SD card manager (SDMMC 1-bit mode)
 *
 * Pin mapping (Waveshare ESP32-S3-Touch-LCD-7"):
 *   SD_CLK  = GPIO 12  (SDMMC CLK / was SPI SCLK)
 *   SD_CMD  = GPIO 11  (SDMMC CMD / was SPI MOSI)
 *   SD_D0   = GPIO 13  (SDMMC D0  / was SPI MISO)
 *   SD_D3   = CH422G IO3 (held HIGH via I2C expander in main.cpp)
 *
 * Note: SD_CS/D3 line is driven HIGH by the CH422G I/O expander (WR_IO=0xFF).
 * HIGH on D3 during power-up tells the SD card to use SD bus mode, not SPI mode.
 * SD_MMC in 1-bit mode does NOT require a CS pin — card selection is handled
 * by the CMD protocol.
 *
 * Creates /sessions and /tracks directories on first boot.
 */

#define SD_CLK_PIN   12
#define SD_CMD_PIN   11
#define SD_D0_PIN    13

bool  sd_mgr_init();                              // returns true if card found
bool  sd_mgr_available();

bool  sd_write_file(const char *path, const char *data, size_t len);
bool  sd_append_file(const char *path, const char *data, size_t len);
bool  sd_read_file(const char *path, char *buf, size_t buf_size);
bool  sd_file_exists(const char *path);
bool  sd_delete_file(const char *path);
bool  sd_make_dir(const char *path);
