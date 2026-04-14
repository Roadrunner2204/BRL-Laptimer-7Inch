/**
 * tbrl_loader.cpp — decrypt + parse /sdcard/Tracks.tbrl into g_bundle_tracks.
 *
 * Header layout (identical to .brl, only magic differs):
 *   "TBRL"  | ver(1) | res(3) | iv(16) | size(LE32) | crc32(LE32)
 *   [encrypted AES-256-CBC payload, PKCS7-padded JSON bundle]
 *
 * Bundle JSON schema (see tools/bdb_format.py :: database_to_tbrl_bundle):
 *   {
 *     "format": "t-brl-bundle", "version": 1,
 *     "tracks": [ { name, country, sf:[4], sectors:[ ... ], is_circuit } ]
 *   }
 *
 * Sector entry inside a track is EITHER single-point ({lat,lon,name}) OR
 * 2-point ({lat1,lon1,lat2,lon2,name}). lap_timer decides gate shape
 * based on whether lat2/lon2 are non-zero.
 */

#include "tbrl_loader.h"
#include "../data/track_db.h"
#include "../data/lap_data.h"
#include "sd_mgr.h"

#include "esp_log.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mbedtls/aes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "tbrl_loader";

// Global bundle storage (declared in track_db.h)
TrackDef *g_bundle_tracks      = nullptr;
int       g_bundle_track_count = 0;

// Must match tools/bdb_format.py :: TBRL_KEY (and .brl key — they share the
// same AES-256-CBC key so firmware doesn't need to maintain two of them).
static const uint8_t TBRL_KEY[32] = {
    0x4d, 0x35, 0x82, 0x75, 0xa7, 0xea, 0x02, 0xf8,
    0x1d, 0xf7, 0xc5, 0x68, 0x9c, 0x07, 0x3f, 0x8e,
    0xdb, 0xd4, 0x64, 0x06, 0x6f, 0x56, 0x37, 0x17,
    0xe6, 0xcb, 0xaf, 0x60, 0xad, 0xdb, 0x71, 0x0a,
};

static const char   TBRL_PATH[] = "/sdcard/Tracks.tbrl";
static const uint8_t TBRL_MAGIC[4] = { 'T', 'B', 'R', 'L' };
#define TBRL_HEADER_SIZE   32
#define TBRL_MAX_FILE_SIZE (4 * 1024 * 1024)   // 4 MB safety cap

// ---------------------------------------------------------------------------
// AES-256-CBC decrypt (mbedtls)
// ---------------------------------------------------------------------------
static bool aes_decrypt_cbc(const uint8_t *iv_in, const uint8_t *cipher,
                            size_t cipher_len, uint8_t *plain) {
    if (cipher_len == 0 || cipher_len % 16 != 0) return false;
    uint8_t iv[16];
    memcpy(iv, iv_in, 16);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_dec(&ctx, TBRL_KEY, 256);
    if (ret != 0) { mbedtls_aes_free(&ctx); return false; }
    ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, cipher_len,
                                iv, cipher, plain);
    mbedtls_aes_free(&ctx);
    return ret == 0;
}

static int pkcs7_unpad_len(const uint8_t *data, size_t len) {
    if (len == 0) return -1;
    uint8_t pad = data[len - 1];
    if (pad < 1 || pad > 16 || (size_t)pad > len) return -1;
    for (size_t i = len - pad; i < len; i++)
        if (data[i] != pad) return -1;
    return (int)(len - pad);
}

// ---------------------------------------------------------------------------
// Sector array parse — tolerates both single-point and 2-point shapes.
// ---------------------------------------------------------------------------
static void parse_sectors(cJSON *arr, TrackDef &td) {
    td.sector_count = 0;
    if (!cJSON_IsArray(arr)) return;
    cJSON *sp;
    cJSON_ArrayForEach(sp, arr) {
        if (td.sector_count >= MAX_SECTORS) break;
        SectorLine &sl = td.sectors[td.sector_count];
        memset(&sl, 0, sizeof(sl));

        cJSON *lat1 = cJSON_GetObjectItem(sp, "lat1");
        if (!cJSON_IsNumber(lat1)) lat1 = cJSON_GetObjectItem(sp, "lat");
        cJSON *lon1 = cJSON_GetObjectItem(sp, "lon1");
        if (!cJSON_IsNumber(lon1)) lon1 = cJSON_GetObjectItem(sp, "lon");
        if (!cJSON_IsNumber(lat1) || !cJSON_IsNumber(lon1)) continue;
        sl.lat = lat1->valuedouble;
        sl.lon = lon1->valuedouble;

        cJSON *lat2 = cJSON_GetObjectItem(sp, "lat2");
        cJSON *lon2 = cJSON_GetObjectItem(sp, "lon2");
        if (cJSON_IsNumber(lat2)) sl.lat2 = lat2->valuedouble;
        if (cJSON_IsNumber(lon2)) sl.lon2 = lon2->valuedouble;

        cJSON *name = cJSON_GetObjectItem(sp, "name");
        if (cJSON_IsString(name))
            strncpy(sl.name, name->valuestring, SECTOR_NAME_LEN - 1);
        else
            snprintf(sl.name, SECTOR_NAME_LEN, "S%u",
                     (unsigned)(td.sector_count + 1));

        td.sector_count++;
    }
}

// ---------------------------------------------------------------------------
// Parse a single track entry from the bundle.
// ---------------------------------------------------------------------------
static bool parse_track(cJSON *t, TrackDef &td) {
    memset(&td, 0, sizeof(td));
    td.user_created = false;   // bundle tracks are read-only

    cJSON *j = cJSON_GetObjectItem(t, "name");
    if (!cJSON_IsString(j) || j->valuestring[0] == 0) return false;
    strncpy(td.name, j->valuestring, sizeof(td.name) - 1);

    j = cJSON_GetObjectItem(t, "country");
    if (cJSON_IsString(j))
        strncpy(td.country, j->valuestring, sizeof(td.country) - 1);

    j = cJSON_GetObjectItem(t, "length_km");
    if (cJSON_IsNumber(j)) td.length_km = (float)j->valuedouble;

    j = cJSON_GetObjectItem(t, "is_circuit");
    td.is_circuit = cJSON_IsTrue(j) || !cJSON_IsBool(j);  // default true

    cJSON *sf = cJSON_GetObjectItem(t, "sf");
    if (!cJSON_IsArray(sf) || cJSON_GetArraySize(sf) < 4) return false;
    td.sf_lat1 = cJSON_GetArrayItem(sf, 0)->valuedouble;
    td.sf_lon1 = cJSON_GetArrayItem(sf, 1)->valuedouble;
    td.sf_lat2 = cJSON_GetArrayItem(sf, 2)->valuedouble;
    td.sf_lon2 = cJSON_GetArrayItem(sf, 3)->valuedouble;

    if (!td.is_circuit) {
        cJSON *fin = cJSON_GetObjectItem(t, "fin");
        if (cJSON_IsArray(fin) && cJSON_GetArraySize(fin) >= 4) {
            td.fin_lat1 = cJSON_GetArrayItem(fin, 0)->valuedouble;
            td.fin_lon1 = cJSON_GetArrayItem(fin, 1)->valuedouble;
            td.fin_lat2 = cJSON_GetArrayItem(fin, 2)->valuedouble;
            td.fin_lon2 = cJSON_GetArrayItem(fin, 3)->valuedouble;
        }
    }

    parse_sectors(cJSON_GetObjectItem(t, "sectors"), td);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
extern "C" void tbrl_loader_unload(void) {
    if (g_bundle_tracks) { free(g_bundle_tracks); g_bundle_tracks = nullptr; }
    g_bundle_track_count = 0;
}

extern "C" int tbrl_loader_load_default(void) {
    tbrl_loader_unload();

    struct stat st;
    if (stat(TBRL_PATH, &st) != 0) {
        ESP_LOGI(TAG, "No %s — bundle tier stays empty", TBRL_PATH);
        return 0;
    }
    if (st.st_size < TBRL_HEADER_SIZE || st.st_size > TBRL_MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "%s has invalid size: %lld", TBRL_PATH,
                 (long long)st.st_size);
        return 0;
    }

    // Full-file read — stays in heap temporarily, freed before track alloc.
    uint8_t *file = (uint8_t *)heap_caps_malloc(st.st_size,
                                                MALLOC_CAP_SPIRAM);
    if (!file) {
        ESP_LOGE(TAG, "oom reading %lld byte bundle", (long long)st.st_size);
        return 0;
    }
    FILE *fp = fopen(TBRL_PATH, "rb");
    if (!fp) { free(file); return 0; }
    size_t got = fread(file, 1, st.st_size, fp);
    fclose(fp);
    if ((long long)got != st.st_size) {
        ESP_LOGE(TAG, "short read: %zu / %lld", got, (long long)st.st_size);
        free(file);
        return 0;
    }

    if (memcmp(file, TBRL_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "bad magic: %02x%02x%02x%02x",
                 file[0], file[1], file[2], file[3]);
        free(file);
        return 0;
    }

    const uint8_t *iv    = file + 8;
    uint32_t cipher_len  = 0;
    uint32_t expected_crc = 0;
    memcpy(&cipher_len,  file + 24, 4);
    memcpy(&expected_crc, file + 28, 4);

    if (cipher_len == 0 ||
        cipher_len % 16 != 0 ||
        (size_t)(TBRL_HEADER_SIZE + cipher_len) > (size_t)st.st_size) {
        ESP_LOGE(TAG, "invalid cipher_len=%u", (unsigned)cipher_len);
        free(file);
        return 0;
    }

    const uint8_t *cipher = file + TBRL_HEADER_SIZE;
    uint32_t actual_crc = esp_crc32_le(0, cipher, cipher_len);
    if (actual_crc != expected_crc) {
        ESP_LOGE(TAG, "crc32 mismatch: have %08x expected %08x",
                 (unsigned)actual_crc, (unsigned)expected_crc);
        free(file);
        return 0;
    }

    uint8_t *plain = (uint8_t *)heap_caps_malloc(cipher_len,
                                                 MALLOC_CAP_SPIRAM);
    if (!plain) {
        ESP_LOGE(TAG, "oom for plaintext buf");
        free(file);
        return 0;
    }
    // HW-AES on ESP32-P4 needs contiguous DMA-capable internal DRAM for
    // its descriptor array; right after switching to STA the heap is
    // fragmented (LVGL + WebView + WiFi/TLS all hot) and the first call
    // regularly fails with "Failed to allocate memory for the array of
    // DMA descriptors". Retry a few times — heap defrags between tries.
    bool dec_ok = false;
    for (int attempt = 1; attempt <= 4; attempt++) {
        if (aes_decrypt_cbc(iv, cipher, cipher_len, plain)) {
            dec_ok = true;
            break;
        }
        ESP_LOGW(TAG, "AES attempt %d/4 failed, retrying...", attempt);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!dec_ok) {
        ESP_LOGE(TAG, "AES decrypt failed after retries");
        free(plain);
        free(file);
        return 0;
    }
    free(file);   // ciphertext no longer needed

    int plain_len = pkcs7_unpad_len(plain, cipher_len);
    if (plain_len <= 0) {
        ESP_LOGE(TAG, "bad PKCS7 padding");
        free(plain);
        return 0;
    }
    plain[plain_len] = 0;   // safe: we allocated cipher_len ≥ plain_len+1

    cJSON *root = cJSON_ParseWithLength((const char *)plain, plain_len);
    free(plain);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return 0;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "tracks");
    int n_raw = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (n_raw <= 0) {
        ESP_LOGW(TAG, "bundle has no tracks array");
        cJSON_Delete(root);
        return 0;
    }

    // Allocate tracks array in PSRAM; final count may shrink if entries
    // fail parse_track(). Over-allocate to n_raw and trim.
    TrackDef *arr_td = (TrackDef *)heap_caps_calloc(
        n_raw, sizeof(TrackDef), MALLOC_CAP_SPIRAM);
    if (!arr_td) {
        ESP_LOGE(TAG, "oom for %d track slots", n_raw);
        cJSON_Delete(root);
        return 0;
    }

    int got_count = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, arr) {
        if (got_count >= n_raw) break;
        if (parse_track(t, arr_td[got_count])) got_count++;
    }
    cJSON_Delete(root);

    if (got_count == 0) {
        ESP_LOGW(TAG, "no valid tracks after parse");
        free(arr_td);
        return 0;
    }

    // Shrink to actual count to free excess PSRAM.
    if (got_count < n_raw) {
        TrackDef *shrunk = (TrackDef *)heap_caps_realloc(
            arr_td, got_count * sizeof(TrackDef), MALLOC_CAP_SPIRAM);
        if (shrunk) arr_td = shrunk;  // keep original if realloc fails
    }

    g_bundle_tracks      = arr_td;
    g_bundle_track_count = got_count;
    ESP_LOGI(TAG, "Loaded %d tracks from Tracks.tbrl (%d raw)",
             got_count, n_raw);
    return got_count;
}
