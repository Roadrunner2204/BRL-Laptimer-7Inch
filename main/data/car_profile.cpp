/**
 * car_profile.cpp -- BRL vehicle profile parser
 *
 * Decrypts AES-256-CBC encrypted .brl files from SD card,
 * parses the JSON payload into CarProfile struct.
 */

#include "car_profile.h"
#include "../storage/sd_mgr.h"
#include "compat.h"
#include "cJSON.h"

#include "mbedtls/aes.h"
#include "esp_crc.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdlib.h>
#include <dirent.h>

static const char *TAG = "car_profile";

// ---------------------------------------------------------------------------
// AES-256 key (must match brl_format.py BRL_KEY)
// ---------------------------------------------------------------------------
static const uint8_t BRL_AES_KEY[32] = {
    0x4d, 0x35, 0x82, 0x75, 0xa7, 0xea, 0x02, 0xf8,
    0x1d, 0xf7, 0xc5, 0x68, 0x9c, 0x07, 0x3f, 0x8e,
    0xdb, 0xd4, 0x64, 0x06, 0x6f, 0x56, 0x37, 0x17,
    0xe6, 0xcb, 0xaf, 0x60, 0xad, 0xdb, 0x71, 0x0a
};

static const uint8_t BRL_MAGIC[4] = { 'B', 'R', 'L', 0x01 };
#define BRL_HEADER_SIZE 32
#define NVS_CAR_NS      "car_cfg"
#define MAX_BRL_FILE    (64 * 1024)  // 64 KB max

// ---------------------------------------------------------------------------
// Global profile
// ---------------------------------------------------------------------------
CarProfile g_car_profile = {};

// ---------------------------------------------------------------------------
// AES-256-CBC decrypt (mbedtls)
// ---------------------------------------------------------------------------
static bool aes_decrypt_cbc(const uint8_t *key, const uint8_t *iv_in,
                            const uint8_t *cipher, size_t cipher_len,
                            uint8_t *plain)
{
    if (cipher_len % 16 != 0) return false;

    uint8_t iv[16];
    memcpy(iv, iv_in, 16);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int ret = mbedtls_aes_setkey_dec(&ctx, key, 256);
    if (ret != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }

    ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, cipher_len, iv, cipher, plain);
    mbedtls_aes_free(&ctx);
    return (ret == 0);
}

// Remove PKCS7 padding, return actual data length
static int pkcs7_unpad(uint8_t *data, size_t len)
{
    if (len == 0) return -1;
    uint8_t pad = data[len - 1];
    if (pad < 1 || pad > 16 || (size_t)pad > len) return -1;
    for (size_t i = len - pad; i < len; i++) {
        if (data[i] != pad) return -1;
    }
    return (int)(len - pad);
}

// ---------------------------------------------------------------------------
// Parse sensors array from cJSON
// ---------------------------------------------------------------------------
static void parse_sensors(cJSON *arr, CarProfile *p)
{
    p->sensor_count = 0;
    if (!cJSON_IsArray(arr)) return;

    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (p->sensor_count >= CAR_MAX_SENSORS) break;
        CarSensor *s = &p->sensors[p->sensor_count];
        memset(s, 0, sizeof(*s));

        cJSON *j;

        j = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(j))
            strncpy(s->name, j->valuestring, CAR_NAME_LEN - 1);

        j = cJSON_GetObjectItem(item, "can_id");
        if (cJSON_IsString(j))
            s->can_id = (uint32_t)strtoul(j->valuestring, NULL, 16);

        j = cJSON_GetObjectItem(item, "proto");
        if (cJSON_IsString(j)) s->proto = (uint8_t)atoi(j->valuestring);

        j = cJSON_GetObjectItem(item, "start");
        if (cJSON_IsString(j)) s->start = (uint8_t)atoi(j->valuestring);

        j = cJSON_GetObjectItem(item, "len");
        if (cJSON_IsString(j)) s->len = (uint8_t)atoi(j->valuestring);

        j = cJSON_GetObjectItem(item, "unsigned");
        if (cJSON_IsString(j)) s->is_unsigned = (uint8_t)atoi(j->valuestring);

        j = cJSON_GetObjectItem(item, "scale");
        if (cJSON_IsString(j)) s->scale = (float)atof(j->valuestring);

        j = cJSON_GetObjectItem(item, "offset");
        if (cJSON_IsString(j)) s->offset = (float)atof(j->valuestring);

        j = cJSON_GetObjectItem(item, "min");
        if (cJSON_IsString(j)) s->min_val = (float)atof(j->valuestring);

        j = cJSON_GetObjectItem(item, "max");
        if (cJSON_IsString(j)) s->max_val = (float)atof(j->valuestring);

        j = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(j)) s->type = (uint8_t)atoi(j->valuestring);

        j = cJSON_GetObjectItem(item, "slot");
        if (cJSON_IsNumber(j)) s->slot = (uint8_t)j->valuedouble;

        // Skip sensors with empty name or CAN ID 0xFFF (virtual/analog)
        if (strlen(s->name) == 0 || s->can_id == 0xFFF) continue;

        p->sensor_count++;
    }
}

// ---------------------------------------------------------------------------
// car_profile_load_into -- read & decrypt .brl from SD into provided struct
// ---------------------------------------------------------------------------
bool car_profile_load_into(const char *filename, CarProfile *dst)
{
    if (!dst) return false;
    dst->loaded = false;
    memset(dst, 0, sizeof(*dst));

    if (!sd_mgr_available()) {
        log_e("SD not available");
        return false;
    }

    // Build path
    char path[64];
    snprintf(path, sizeof(path), "/cars/%s", filename);

    // Read entire file
    uint8_t *file_buf = (uint8_t*)malloc(MAX_BRL_FILE);
    if (!file_buf) {
        log_e("malloc failed for BRL file");
        return false;
    }

    // Use sd_read_file (reads as text with null terminator)
    // For binary, we need to read raw via fopen
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/sdcard%s", path);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        log_e("Cannot open %s", full_path);
        free(file_buf);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < BRL_HEADER_SIZE || file_size > MAX_BRL_FILE) {
        log_e("BRL file size invalid: %ld", file_size);
        fclose(f);
        free(file_buf);
        return false;
    }

    size_t read = fread(file_buf, 1, (size_t)file_size, f);
    fclose(f);

    if ((long)read != file_size) {
        log_e("Read error: got %u, expected %ld", (unsigned)read, file_size);
        free(file_buf);
        return false;
    }

    // Verify magic
    if (memcmp(file_buf, BRL_MAGIC, 4) != 0) {
        log_e("Invalid BRL magic");
        free(file_buf);
        return false;
    }

    // Parse header
    const uint8_t *iv = file_buf + 8;
    uint32_t payload_size;
    uint32_t expected_crc;
    memcpy(&payload_size, file_buf + 24, 4); // LE
    memcpy(&expected_crc, file_buf + 28, 4); // LE

    if (BRL_HEADER_SIZE + payload_size > (uint32_t)file_size) {
        log_e("Payload size exceeds file");
        free(file_buf);
        return false;
    }

    const uint8_t *encrypted = file_buf + BRL_HEADER_SIZE;

    // Verify CRC32
    uint32_t actual_crc = esp_crc32_le(0, encrypted, payload_size);
    if (actual_crc != expected_crc) {
        log_e("CRC32 mismatch: expected %08lX, got %08lX", (unsigned long)expected_crc, (unsigned long)actual_crc);
        free(file_buf);
        return false;
    }

    // Decrypt
    uint8_t *plain = (uint8_t*)malloc(payload_size);
    if (!plain) {
        log_e("malloc failed for decrypt");
        free(file_buf);
        return false;
    }

    if (!aes_decrypt_cbc(BRL_AES_KEY, iv, encrypted, payload_size, plain)) {
        log_e("AES decrypt failed");
        free(plain);
        free(file_buf);
        return false;
    }

    free(file_buf); // done with raw file

    // Remove PKCS7 padding
    int json_len = pkcs7_unpad(plain, payload_size);
    if (json_len < 0) {
        log_e("PKCS7 unpad failed");
        free(plain);
        return false;
    }
    plain[json_len] = '\0';

    // Parse JSON
    cJSON *doc = cJSON_Parse((const char*)plain);
    free(plain);

    if (!doc) {
        log_e("JSON parse failed");
        return false;
    }

    // Extract vehicle info
    cJSON *j;
    CarProfile *p = dst;

    j = cJSON_GetObjectItem(doc, "engine");
    if (cJSON_IsString(j)) strncpy(p->engine, j->valuestring, CAR_ENGINE_LEN - 1);

    j = cJSON_GetObjectItem(doc, "name");
    if (cJSON_IsString(j)) strncpy(p->name, j->valuestring, CAR_NAME_LEN - 1);

    j = cJSON_GetObjectItem(doc, "make");
    if (cJSON_IsString(j)) strncpy(p->make, j->valuestring, CAR_NAME_LEN - 1);

    j = cJSON_GetObjectItem(doc, "model");
    if (cJSON_IsString(j)) strncpy(p->model, j->valuestring, CAR_MODEL_LEN - 1);

    j = cJSON_GetObjectItem(doc, "year_from");
    if (cJSON_IsNumber(j)) p->year_from = (uint16_t)j->valuedouble;

    j = cJSON_GetObjectItem(doc, "year_to");
    if (cJSON_IsNumber(j)) p->year_to = (uint16_t)j->valuedouble;

    j = cJSON_GetObjectItem(doc, "can");
    if (cJSON_IsString(j)) strncpy(p->can_bus, j->valuestring, sizeof(p->can_bus) - 1);

    j = cJSON_GetObjectItem(doc, "bitrate");
    if (cJSON_IsNumber(j)) p->bitrate = (uint16_t)j->valuedouble;

    // Parse sensors
    parse_sensors(cJSON_GetObjectItem(doc, "sensors"), p);

    cJSON_Delete(doc);

    p->loaded = true;
    log_i("Car profile loaded: %s %s %s (%d sensors, %s %d kBit/s)",
          p->make, p->engine, p->name, p->sensor_count,
          p->can_bus, p->bitrate);

    return true;
}

// ---------------------------------------------------------------------------
// car_profile_load -- thin wrapper that loads into the global g_car_profile
// ---------------------------------------------------------------------------
bool car_profile_load(const char *filename)
{
    return car_profile_load_into(filename, &g_car_profile);
}

// ---------------------------------------------------------------------------
// car_profile_list -- list .brl files on SD
// ---------------------------------------------------------------------------
int car_profile_list(char filenames[][CAR_NAME_LEN], int max_count)
{
    if (!sd_mgr_available()) return 0;

    DIR *dir = opendir("/sdcard/cars");
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_count) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5) continue;
        if (strcmp(ent->d_name + nlen - 4, ".brl") != 0) continue;

        strncpy(filenames[count], ent->d_name, CAR_NAME_LEN - 1);
        filenames[count][CAR_NAME_LEN - 1] = '\0';
        count++;
    }
    closedir(dir);
    return count;
}

// ---------------------------------------------------------------------------
// NVS persistence for active profile
// ---------------------------------------------------------------------------
void car_profile_get_active(char *buf, int buf_len)
{
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_CAR_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = (size_t)buf_len;
    nvs_get_str(h, "active", buf, &len);
    nvs_close(h);
}

void car_profile_set_active(const char *filename)
{
    nvs_handle_t h;
    if (nvs_open(NVS_CAR_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "active", filename);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// HTTP download helpers
// ---------------------------------------------------------------------------
#include "esp_http_client.h"

#define BRL_SERVER_BASE  "https://downloads.bavarian-racelabs.com/cars/"

// Buffer for HTTP response
static char   *s_http_buf = nullptr;
static size_t  s_http_len = 0;
static size_t  s_http_cap = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_http_buf && s_http_len + evt->data_len < s_http_cap) {
            memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
            s_http_len += evt->data_len;
        }
    }
    return ESP_OK;
}

int car_profile_fetch_list(CarProfileEntry *entries, int max_count)
{
    s_http_cap = 4096;
    s_http_len = 0;
    s_http_buf = (char*)malloc(s_http_cap);
    if (!s_http_buf) return 0;

    esp_http_client_config_t cfg = {};
    cfg.url = BRL_SERVER_BASE "list.txt";
    cfg.event_handler = http_event_handler;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        log_e("Fetch list failed: err=%s status=%d", esp_err_to_name(err), status);
        free(s_http_buf);
        s_http_buf = nullptr;
        return 0;
    }

    s_http_buf[s_http_len] = '\0';

    // Parse line by line: "filename.brl;Make;Display Name"
    int count = 0;
    char *line = s_http_buf;
    while (*line && count < max_count) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Trim \r
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') line[ll - 1] = '\0';

        // Parse semicolon-separated fields
        char *sep1 = strchr(line, ';');
        if (sep1) {
            *sep1 = '\0';
            char *make_str = sep1 + 1;
            char *sep2 = strchr(make_str, ';');
            char *display_str = nullptr;
            if (sep2) {
                *sep2 = '\0';
                display_str = sep2 + 1;
            }

            // First field must be a .brl filename
            ll = strlen(line);
            if (ll >= 5 && strcmp(line + ll - 4, ".brl") == 0) {
                strncpy(entries[count].filename, line, CAR_NAME_LEN - 1);
                entries[count].filename[CAR_NAME_LEN - 1] = '\0';
                strncpy(entries[count].make, make_str, CAR_NAME_LEN - 1);
                entries[count].make[CAR_NAME_LEN - 1] = '\0';
                if (display_str) {
                    strncpy(entries[count].display, display_str, CAR_NAME_LEN - 1);
                } else {
                    // Fallback: filename without .brl
                    strncpy(entries[count].display, line, CAR_NAME_LEN - 1);
                    char *dot = strrchr(entries[count].display, '.');
                    if (dot) *dot = '\0';
                }
                entries[count].display[CAR_NAME_LEN - 1] = '\0';
                count++;
            }
        } else {
            // Legacy format: just filename
            ll = strlen(line);
            if (ll >= 5 && strcmp(line + ll - 4, ".brl") == 0) {
                strncpy(entries[count].filename, line, CAR_NAME_LEN - 1);
                entries[count].filename[CAR_NAME_LEN - 1] = '\0';
                entries[count].make[0] = '\0';
                strncpy(entries[count].display, line, CAR_NAME_LEN - 1);
                char *dot = strrchr(entries[count].display, '.');
                if (dot) *dot = '\0';
                entries[count].display[CAR_NAME_LEN - 1] = '\0';
                count++;
            }
        }

        if (!nl) break;
        line = nl + 1;
    }

    free(s_http_buf);
    s_http_buf = nullptr;
    log_i("Server profile list: %d profiles", count);
    return count;
}

bool car_profile_download(const char *filename)
{
    if (!sd_mgr_available() || !filename) return false;

    char url[128];
    snprintf(url, sizeof(url), BRL_SERVER_BASE "%s", filename);

    s_http_cap = MAX_BRL_FILE;
    s_http_len = 0;
    s_http_buf = (char*)malloc(s_http_cap);
    if (!s_http_buf) {
        log_e("malloc failed for download");
        return false;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_event_handler;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        log_e("Download %s failed: err=%s status=%d", filename, esp_err_to_name(err), status);
        free(s_http_buf);
        s_http_buf = nullptr;
        return false;
    }

    // Verify BRL magic before saving
    if (s_http_len < BRL_HEADER_SIZE || memcmp(s_http_buf, BRL_MAGIC, 4) != 0) {
        log_e("Downloaded data is not a valid BRL file");
        free(s_http_buf);
        s_http_buf = nullptr;
        return false;
    }

    // Save to SD
    char sd_path[64];
    snprintf(sd_path, sizeof(sd_path), "/sdcard/cars/%s", filename);
    FILE *f = fopen(sd_path, "wb");
    if (!f) {
        log_e("Cannot write %s", sd_path);
        free(s_http_buf);
        s_http_buf = nullptr;
        return false;
    }
    fwrite(s_http_buf, 1, s_http_len, f);
    fclose(f);

    free(s_http_buf);
    s_http_buf = nullptr;

    log_i("Profile downloaded: %s (%u bytes)", filename, (unsigned)s_http_len);
    return true;
}

bool car_profile_delete(const char *filename)
{
    if (!sd_mgr_available() || !filename) return false;

    char path[64];
    snprintf(path, sizeof(path), "/cars/%s", filename);

    if (!sd_file_exists(path)) return false;

    bool ok = sd_delete_file(path);
    if (ok) {
        log_i("Profile deleted: %s", filename);
        // If deleted profile was active, clear active
        char active[CAR_NAME_LEN] = {};
        car_profile_get_active(active, sizeof(active));
        if (strcmp(active, filename) == 0) {
            car_profile_set_active("");
            g_car_profile.loaded = false;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Find sensor by name
// ---------------------------------------------------------------------------
const CarSensor *car_profile_find_sensor(const char *name)
{
    if (!g_car_profile.loaded || !name) return NULL;
    for (int i = 0; i < g_car_profile.sensor_count; i++) {
        if (strcmp(g_car_profile.sensors[i].name, name) == 0)
            return &g_car_profile.sensors[i];
    }
    return NULL;
}
