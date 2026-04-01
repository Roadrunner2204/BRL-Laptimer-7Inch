/**
 * sd_mgr.cpp — SD card manager
 */

#include "sd_mgr.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

static SPIClass s_spi(HSPI);
static bool     s_available = false;

bool sd_mgr_init() {
    s_spi.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    if (!SD.begin(SD_CS_PIN, s_spi, 4000000)) {
        Serial.println("[SD] No card or init failed");
        s_available = false;
        g_state.sd_available = false;
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD] No SD card attached");
        s_available = false;
        g_state.sd_available = false;
        return false;
    }

    // Ensure sessions directory exists
    if (!SD.exists("/sessions")) {
        SD.mkdir("/sessions");
    }

    s_available          = true;
    g_state.sd_available = true;
    Serial.printf("[SD] Card ready. Size: %llu MB\n",
                  SD.cardSize() / (1024 * 1024));
    return true;
}

bool sd_mgr_available() { return s_available; }

bool sd_write_file(const char *path, const char *data, size_t len) {
    if (!s_available) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write((const uint8_t *)data, len);
    f.close();
    return written == len;
}

bool sd_append_file(const char *path, const char *data, size_t len) {
    if (!s_available) return false;
    File f = SD.open(path, FILE_APPEND);
    if (!f) return false;
    size_t written = f.write((const uint8_t *)data, len);
    f.close();
    return written == len;
}

bool sd_read_file(const char *path, char *buf, size_t buf_size) {
    if (!s_available) return false;
    File f = SD.open(path);
    if (!f) return false;
    size_t n = f.readBytes(buf, buf_size - 1);
    buf[n] = '\0';
    f.close();
    return true;
}

bool sd_file_exists(const char *path) {
    if (!s_available) return false;
    return SD.exists(path);
}

bool sd_delete_file(const char *path) {
    if (!s_available) return false;
    return SD.remove(path);
}

bool sd_make_dir(const char *path) {
    if (!s_available) return false;
    return SD.mkdir(path);
}
