/**
 * sd_mgr.cpp — SD card manager (SDMMC 1-bit mode)
 *
 * Uses ESP32-S3 SDMMC host controller in 1-bit mode.
 * No CS pin needed — SD bus protocol handles card selection.
 * SD D3 line is held HIGH by CH422G IO3 (set in main.cpp before this runs).
 */

#include "sd_mgr.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <SD_MMC.h>

static bool s_available = false;

bool sd_mgr_init() {
    // Configure SDMMC pins: CLK=12, CMD=11, D0=13
    // 1-bit mode (true) — D3 held HIGH externally by CH422G
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);

    if (!SD_MMC.begin("/sdcard", true /* 1-bit */, false /* no format on fail */)) {
        log_e("[SD] No card or init failed");
        s_available = false;
        g_state.sd_available = false;
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        log_e("[SD] No SD card attached");
        s_available = false;
        g_state.sd_available = false;
        return false;
    }

    // Ensure required directories exist
    if (!SD_MMC.exists("/sessions")) SD_MMC.mkdir("/sessions");
    if (!SD_MMC.exists("/tracks"))   SD_MMC.mkdir("/tracks");

    s_available          = true;
    g_state.sd_available = true;
    log_e("[SD] Card ready. Type:%d  Size:%llu MB",
          cardType, SD_MMC.cardSize() / (1024 * 1024));
    return true;
}

bool sd_mgr_available() { return s_available; }

bool sd_write_file(const char *path, const char *data, size_t len) {
    if (!s_available) return false;
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write((const uint8_t *)data, len);
    f.close();
    return written == len;
}

bool sd_append_file(const char *path, const char *data, size_t len) {
    if (!s_available) return false;
    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f) return false;
    size_t written = f.write((const uint8_t *)data, len);
    f.close();
    return written == len;
}

bool sd_read_file(const char *path, char *buf, size_t buf_size) {
    if (!s_available) return false;
    File f = SD_MMC.open(path);
    if (!f) return false;
    size_t n = f.readBytes(buf, buf_size - 1);
    buf[n] = '\0';
    f.close();
    return true;
}

bool sd_file_exists(const char *path) {
    if (!s_available) return false;
    return SD_MMC.exists(path);
}

bool sd_delete_file(const char *path) {
    if (!s_available) return false;
    return SD_MMC.remove(path);
}

bool sd_make_dir(const char *path) {
    if (!s_available) return false;
    return SD_MMC.mkdir(path);
}
