/**
 * sd_mgr.cpp -- SD card manager for ESP-IDF / ESP32-P4
 *
 * Uses esp_vfs_fat_sdmmc_mount() to mount the SD card via SDMMC (SDIO)
 * and exposes simple POSIX-based file helpers to the rest of the app.
 *
 * Mount point: /sdcard
 * Callers may pass paths with or without the "/sdcard" prefix; the
 * helper functions prepend it automatically when missing.
 */

#include "sd_mgr.h"
#include "compat.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static const char *TAG = "sd_mgr";

#define SD_MOUNT_POINT "/sdcard"
#define MAX_PATH_LEN   256

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

// ---------------------------------------------------------------------------
// Helper: build full path by prepending mount point when needed
// ---------------------------------------------------------------------------
static void full_path(char *out, size_t len, const char *path)
{
    if (strncmp(path, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) == 0) {
        strncpy(out, path, len - 1);
        out[len - 1] = '\0';
    } else {
        snprintf(out, len, "%s%s", SD_MOUNT_POINT, path);
    }
}

// ---------------------------------------------------------------------------
// sd_mgr_init -- mount SD card via SDMMC and create default directories
// ---------------------------------------------------------------------------
bool sd_mgr_init(void)
{
    if (s_mounted) {
        log_i("SD card already mounted");
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;              // Waveshare board TF slot is on Slot 0
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // Power the SD card via on-chip LDO (channel 4)
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        log_e("SD LDO power control init failed: %s", esp_err_to_name(ret));
        return false;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    // Slot 0 uses IO MUX — no pin assignment needed
    sdmmc_slot_config_t slot = {
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    log_i("Mounting SD card at " SD_MOUNT_POINT " ...");

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                   &mount_config, &s_card);

    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_TIMEOUT) {
        // Fall back to 1-bit mode
        log_w("4-bit mount failed (0x%x), retrying with 1-bit", ret);
        slot.width = 1;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                       &mount_config, &s_card);
    }

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            log_e("Failed to mount filesystem on SD card");
        } else {
            log_e("SD card init failed: %s (0x%x)", esp_err_to_name(ret), ret);
        }
        s_card    = NULL;
        s_mounted = false;
        return false;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    log_i("SD card mounted successfully");

    // Create default directories (ignore errors if they already exist)
    sd_make_dir("/sessions");
    sd_make_dir("/tracks");
    sd_make_dir("/cars");

    return true;
}

// ---------------------------------------------------------------------------
// sd_mgr_available -- return true if the SD card is mounted
// ---------------------------------------------------------------------------
bool sd_mgr_available(void)
{
    return s_mounted;
}

// ---------------------------------------------------------------------------
// sd_write_file -- write (overwrite) a file
// ---------------------------------------------------------------------------
bool sd_write_file(const char *path, const char *data, size_t len)
{
    if (!s_mounted) return false;

    char fp[MAX_PATH_LEN];
    full_path(fp, sizeof(fp), path);

    FILE *f = fopen(fp, "w");
    if (!f) {
        log_e("fopen(%s, w) failed: %s", fp, strerror(errno));
        return false;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        log_e("fwrite to %s: wrote %u / %u bytes", fp,
              (unsigned)written, (unsigned)len);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sd_append_file -- append data to a file
// ---------------------------------------------------------------------------
bool sd_append_file(const char *path, const char *data, size_t len)
{
    if (!s_mounted) return false;

    char fp[MAX_PATH_LEN];
    full_path(fp, sizeof(fp), path);

    FILE *f = fopen(fp, "a");
    if (!f) {
        log_e("fopen(%s, a) failed: %s", fp, strerror(errno));
        return false;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        log_e("fappend to %s: wrote %u / %u bytes", fp,
              (unsigned)written, (unsigned)len);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sd_read_file -- read an entire file into buf (null-terminated)
//
// Returns true on success.  buf is always null-terminated (up to buf_size-1
// bytes of file content).
// ---------------------------------------------------------------------------
bool sd_read_file(const char *path, char *buf, size_t buf_size)
{
    if (!s_mounted || buf_size == 0) return false;
    buf[0] = '\0';

    char fp[MAX_PATH_LEN];
    full_path(fp, sizeof(fp), path);

    FILE *f = fopen(fp, "r");
    if (!f) {
        log_e("fopen(%s, r) failed: %s", fp, strerror(errno));
        return false;
    }

    size_t total = fread(buf, 1, buf_size - 1, f);
    fclose(f);

    buf[total] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// sd_file_exists -- check if a file or directory exists
// ---------------------------------------------------------------------------
bool sd_file_exists(const char *path)
{
    if (!s_mounted) return false;

    char fp[MAX_PATH_LEN];
    full_path(fp, sizeof(fp), path);

    return (access(fp, F_OK) == 0);
}

// ---------------------------------------------------------------------------
// sd_delete_file -- remove a file
// ---------------------------------------------------------------------------
bool sd_delete_file(const char *path)
{
    if (!s_mounted) return false;

    char fp[MAX_PATH_LEN];
    full_path(fp, sizeof(fp), path);

    if (remove(fp) != 0) {
        log_e("remove(%s) failed: %s", fp, strerror(errno));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sd_make_dir -- create a directory (single level)
// ---------------------------------------------------------------------------
bool sd_make_dir(const char *path)
{
    if (!s_mounted) return false;

    char fp[MAX_PATH_LEN];
    full_path(fp, sizeof(fp), path);

    if (mkdir(fp, 0775) != 0 && errno != EEXIST) {
        log_e("mkdir(%s) failed: %s", fp, strerror(errno));
        return false;
    }
    return true;
}
