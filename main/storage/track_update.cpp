/**
 * track_update.cpp — HTTPS download of Tracks_GER.tbrl / Tracks_EN.tbrl
 * from the BRL server, atomic replace on SD, bundle reload.
 *
 * The exact filenames stay internal; the customer only sees a "UPDATE"
 * button + a progress toast.
 */

#include "track_update.h"
#include "tbrl_loader.h"
#include "sd_mgr.h"
#include "../data/lap_data.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "track_update";

#define TRACKS_SERVER_BASE  "https://downloads.bavarian-racelabs.com/tracks/"
#define TRACKS_MAX_SIZE     (4 * 1024 * 1024)   // 4 MB cap
#define TRACKS_SD_PATH      "/sdcard/Tracks.tbrl"
#define TRACKS_SD_TMP       "/sdcard/Tracks.tbrl.tmp"
#define TRACKS_MAGIC        "TBRL"

// HTTP download buffer (PSRAM — bundle ~1 MB)
static uint8_t *s_buf = nullptr;
static size_t   s_buf_len = 0;
static size_t   s_buf_cap = 0;

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_buf) {
        size_t avail = s_buf_cap - s_buf_len;
        size_t take  = (size_t)evt->data_len < avail ? evt->data_len : avail;
        if (take > 0) {
            memcpy(s_buf + s_buf_len, evt->data, take);
            s_buf_len += take;
        }
    }
    return ESP_OK;
}

static const char *pick_filename(void) {
    // 0 = DE → GER, 1 = EN → EN. Default DE for any unknown value.
    return (g_state.language == 1) ? "Tracks_EN.tbrl"
                                   : "Tracks_GER.tbrl";
}

extern "C" int track_update_run_blocking(void) {
    if (!g_state.sd_available) {
        ESP_LOGE(TAG, "SD not available");
        return -2;
    }

    const char *fname = pick_filename();
    char url[160];
    snprintf(url, sizeof(url), "%s%s", TRACKS_SERVER_BASE, fname);
    ESP_LOGI(TAG, "GET %s", url);

    // Allocate buffer in PSRAM
    s_buf_cap = TRACKS_MAX_SIZE;
    s_buf_len = 0;
    s_buf     = (uint8_t *)heap_caps_malloc(s_buf_cap, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "oom %u KiB", (unsigned)(s_buf_cap / 1024));
        return -1;
    }

    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.event_handler     = http_event_cb;
    cfg.timeout_ms        = 30000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size       = 4096;
    cfg.buffer_size_tx    = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "download failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        free(s_buf); s_buf = nullptr;
        return -1;
    }
    if (s_buf_len < 32 || memcmp(s_buf, TRACKS_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "bad payload (len=%zu, magic=%02x%02x%02x%02x)",
                 s_buf_len, s_buf[0], s_buf[1], s_buf[2], s_buf[3]);
        free(s_buf); s_buf = nullptr;
        return -3;
    }
    ESP_LOGI(TAG, "downloaded %zu bytes", s_buf_len);

    // Atomic replace — write to .tmp then rename.
    FILE *f = fopen(TRACKS_SD_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for write", TRACKS_SD_TMP);
        free(s_buf); s_buf = nullptr;
        return -4;
    }
    size_t wrote = fwrite(s_buf, 1, s_buf_len, f);
    fclose(f);
    free(s_buf); s_buf = nullptr;
    if (wrote != s_buf_len) {
        ESP_LOGE(TAG, "short write: %zu / %zu", wrote, s_buf_len);
        remove(TRACKS_SD_TMP);
        return -4;
    }
    // Ensure the destination doesn't block rename on some FAT configurations.
    struct stat dst_st;
    if (stat(TRACKS_SD_PATH, &dst_st) == 0) remove(TRACKS_SD_PATH);
    if (rename(TRACKS_SD_TMP, TRACKS_SD_PATH) != 0) {
        ESP_LOGE(TAG, "rename tmp → final failed");
        remove(TRACKS_SD_TMP);
        return -4;
    }

    // Repopulate bundle tier from the fresh file
    int n = tbrl_loader_load_default();
    ESP_LOGI(TAG, "update done: %d tracks active", n);
    return n;
}
