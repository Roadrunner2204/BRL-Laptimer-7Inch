/**
 * mic.cpp -- Microphone capture via Waveshare BSP (ES7210 codec + I2S)
 *
 * BSP sequence:
 *   1. bsp_audio_init(NULL)              — allocates I2S TX/RX channels
 *   2. bsp_audio_codec_microphone_init() — returns esp_codec_dev_handle_t
 *   3. esp_codec_dev_open()              — start codec
 *   4. esp_codec_dev_read()              — pull PCM samples
 */

#include "mic.h"
#include "../compat.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "mic";

// Config ----------------------------------------------------------------------
#define MIC_SAMPLE_RATE   22050         // Hz
#define MIC_BITS          16
#define MIC_CHANNELS      2             // ES7210 delivers stereo (left+right mic)
#define MIC_DMA_FRAME     256           // I2S DMA descriptor size (frames)

// Pick a chunk size that is an integer multiple of the DMA frame. Otherwise
// every read straddles a descriptor boundary and we hear a "tick" at ~1/chunk.
// 2048 samples × 2 channels × 2 bytes = 8192 bytes = 16 DMA descriptors per read.
// At 22050 Hz that's ~93 ms per chunk — close to 100 ms, well aligned.
static const uint32_t CHUNK_SAMPLES = 2048;
static const uint32_t CHUNK_BYTES   = CHUNK_SAMPLES * (MIC_BITS / 8) * MIC_CHANNELS;

// State -----------------------------------------------------------------------
static esp_codec_dev_handle_t s_mic_dev = nullptr;
static bool s_initialized = false;
static bool s_codec_open = false;    // tracks esp_codec_dev_open/close pairing
static volatile bool s_running = false;
static mic_chunk_cb_t s_cb = nullptr;
static TaskHandle_t s_task = nullptr;
static int16_t *s_chunk_buf = nullptr;

// ---------------------------------------------------------------------------
static void mic_task(void *arg)
{
    (void)arg;
    log_i("Mic task started (chunk %lu samples = %lu bytes)",
          (unsigned long)CHUNK_SAMPLES, (unsigned long)CHUNK_BYTES);

    while (s_running) {
        if (!s_mic_dev || !s_chunk_buf) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        esp_err_t err = esp_codec_dev_read(s_mic_dev, s_chunk_buf, CHUNK_BYTES);
        if (err != ESP_OK) {
            log_w("codec read err: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (s_cb) s_cb(s_chunk_buf, CHUNK_SAMPLES);
    }

    log_i("Mic task exit");
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ===========================================================================
// Public API
// ===========================================================================

bool mic_init(void)
{
    if (s_initialized) return true;

    // BSP sets up I2S TX+RX channels (needed for codec clocks)
    esp_err_t err = bsp_audio_init(nullptr);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        log_e("bsp_audio_init: %s", esp_err_to_name(err));
        return false;
    }

    s_mic_dev = bsp_audio_codec_microphone_init();
    if (!s_mic_dev) {
        log_e("bsp_audio_codec_microphone_init returned NULL");
        return false;
    }

    // Chunk buffer in PSRAM
    s_chunk_buf = (int16_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_chunk_buf) {
        log_e("chunk buf alloc failed (%lu B)", (unsigned long)CHUNK_BYTES);
        return false;
    }

    // Set gain to a reasonable default (0-100; 40 = moderate)
    esp_codec_dev_set_in_gain(s_mic_dev, 40.0f);

    s_initialized = true;
    log_i("Mic initialized: %d Hz, %d-bit, %d ch, chunk=%lu samples (%lu B)",
          MIC_SAMPLE_RATE, MIC_BITS, MIC_CHANNELS,
          (unsigned long)CHUNK_SAMPLES, (unsigned long)CHUNK_BYTES);
    return true;
}

bool mic_start(mic_chunk_cb_t cb)
{
    if (!s_initialized && !mic_init()) return false;
    if (s_running) return true;

    // Open the codec device exactly once and keep it open across start/stop
    // cycles. esp_codec_dev_open() internally calls i2s_channel_disable() as
    // a defensive safety step — after a close() that warning gets logged for
    // the already-disabled channel. Keeping it open silences that cleanly and
    // also saves ~50 ms of codec re-init on every recording start.
    if (!s_codec_open) {
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate     = MIC_SAMPLE_RATE;
        fs.channel         = MIC_CHANNELS;
        fs.bits_per_sample = MIC_BITS;
        fs.channel_mask    = 0;
        esp_err_t err = esp_codec_dev_open(s_mic_dev, &fs);
        if (err != ESP_OK) {
            log_e("codec open: %s", esp_err_to_name(err));
            return false;
        }
        s_codec_open = true;
    }

    s_cb = cb;
    s_running = true;
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, nullptr, 5, &s_task, 0);
    log_i("Mic capture started");
    return true;
}

void mic_stop(void)
{
    if (!s_running) return;
    s_running = false;
    // Give the task a moment to exit cleanly. We do NOT call
    // esp_codec_dev_close() here — the codec stays open across
    // record sessions to avoid I2S channel re-init warnings.
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    s_cb = nullptr;
    log_i("Mic capture stopped");
}

uint32_t mic_sample_rate(void) { return MIC_SAMPLE_RATE; }
uint8_t  mic_channels(void)    { return MIC_CHANNELS; }
