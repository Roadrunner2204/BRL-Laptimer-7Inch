/**
 * sd_mgr.c — SDMMC mount + filesystem helpers for the cam module.
 *
 * Uses the ESP-IDF SDMMC host directly (no BSP — DFR1172 doesn't ship
 * its own BSP component). Pin defaults assume the DFR1172 onboard
 * microSD slot. Move to a Kconfig once the board layout is finalised.
 */

#include "sd_mgr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <string.h>

static const char *TAG = "sd_mgr";

#define SD_MOUNT_POINT  "/sdcard"

static bool s_available = false;
static sdmmc_card_t *s_card = NULL;

void sd_mgr_init(void)
{
    ESP_LOGI(TAG, "init");

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
        .disk_status_check_enable = false,
    };

    sdmmc_host_t        host     = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 4;     /* 4-bit mode for full SDIO bandwidth */

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        s_available = false;
        return;
    }
    s_available = true;
    sdmmc_card_print_info(stdout, s_card);
}

bool sd_mgr_available(void) { return s_available; }

uint64_t sd_mgr_total_bytes(void)
{
    if (!s_available) return 0;
    struct statvfs st;
    if (statvfs(SD_MOUNT_POINT, &st) != 0) return 0;
    return (uint64_t)st.f_blocks * st.f_frsize;
}

uint64_t sd_mgr_free_bytes(void)
{
    if (!s_available) return 0;
    struct statvfs st;
    if (statvfs(SD_MOUNT_POINT, &st) != 0) return 0;
    return (uint64_t)st.f_bavail * st.f_frsize;
}

uint8_t sd_mgr_free_pct(void)
{
    uint64_t total = sd_mgr_total_bytes();
    if (total == 0) return 0;
    uint64_t free_b = sd_mgr_free_bytes();
    return (uint8_t)((free_b * 100ULL) / total);
}

bool sd_mgr_make_dirs(const char *path)
{
    if (!s_available || !path || !*path) return false;

    /* Build a mutable copy under SD_MOUNT_POINT, walk it component by
     * component, mkdir each one. Tolerates a leading "/sdcard". */
    char buf[160];
    if (strncmp(path, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) == 0) {
        strncpy(buf, path, sizeof(buf) - 1);
    } else {
        snprintf(buf, sizeof(buf), "%s/%s",
                 SD_MOUNT_POINT, (path[0] == '/') ? path + 1 : path);
    }
    buf[sizeof(buf) - 1] = '\0';

    /* Skip the mount-point prefix when walking. */
    char *p = buf + strlen(SD_MOUNT_POINT);
    if (*p == '/') p++;

    while (*p) {
        char *slash = strchr(p, '/');
        if (slash) *slash = '\0';
        if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir(%s): errno=%d", buf, errno);
        }
        if (!slash) break;
        *slash = '/';
        p = slash + 1;
    }

    struct stat st;
    return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}
