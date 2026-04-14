/**
 * track_best.cpp — per-track absolute best lap (NVS).
 *
 * NVS keys are limited to 15 chars. Track names can exceed that, so we
 * hash the name into an 8-char hex key. Collisions are astronomically
 * unlikely given a hobbyist-scale track catalog (<100 tracks).
 */

#include "track_best.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "track_best";
static const char *NS = "trk_best";

// FNV-1a 32-bit — tiny, adequate for unique short hashes
static uint32_t fnv1a32(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) { h ^= (uint8_t)(*s++); h *= 0x01000193u; }
    return h;
}

static void make_key(const char *track_name, char *out15) {
    uint32_t h = fnv1a32(track_name ? track_name : "");
    // 8 hex chars + 't' prefix (NVS keys must start with a letter/_ on some ESP-IDF versions)
    snprintf(out15, 15, "t%08x", (unsigned)h);
}

void track_best_load(const char *track_name, TrackBest *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!track_name || !track_name[0]) return;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;

    char key[15];
    make_key(track_name, key);

    size_t sz = sizeof(TrackBest);
    TrackBest tmp;
    if (nvs_get_blob(h, key, &tmp, &sz) == ESP_OK && sz == sizeof(TrackBest)) {
        *out = tmp;
        out->valid = (tmp.total_ms > 0);
    }
    nvs_close(h);
}

bool track_best_maybe_update(const char *track_name,
                             uint32_t total_ms,
                             const uint32_t sector_ms[MAX_SECTORS],
                             uint8_t sectors_used) {
    if (!track_name || !track_name[0] || total_ms == 0) return false;

    TrackBest existing;
    track_best_load(track_name, &existing);
    if (existing.valid && existing.total_ms <= total_ms) return false;

    TrackBest rec = {};
    rec.total_ms = total_ms;
    rec.sectors_used = sectors_used > MAX_SECTORS ? MAX_SECTORS : sectors_used;
    if (sector_ms) memcpy(rec.sector_ms, sector_ms, sizeof(uint32_t) * MAX_SECTORS);
    rec.valid = true;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed");
        return false;
    }
    char key[15];
    make_key(track_name, key);
    esp_err_t err = nvs_set_blob(h, key, &rec, sizeof(rec));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "New absolute best for '%s': %lu ms",
             track_name, (unsigned long)total_ms);
    return true;
}

void track_best_clear(const char *track_name) {
    if (!track_name || !track_name[0]) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[15];
    make_key(track_name, key);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}
