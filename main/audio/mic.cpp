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
// Mono 16 kHz 16-bit = 32 KB/s SD bandwidth (vs 88 KB/s at stereo 22.05 kHz).
// The reclaimed 56 KB/s goes to video → ~8 more fps at 11 KB frames.
// Sound quality is still fine for in-car narration/ambient recording.
#define MIC_SAMPLE_RATE   16000         // Hz
#define MIC_BITS          16
#define MIC_CHANNELS      1             // Mono (only MIC1 enabled on ES7210)
#define MIC_DMA_FRAME     256           // I2S DMA descriptor size (frames)

// 2048 samples × 1 channel × 2 bytes = 4096 bytes per chunk.
// At 16 kHz that's 128 ms per chunk — same feel as the old stereo config.
static const uint32_t CHUNK_SAMPLES = 2048;
static const uint32_t CHUNK_BYTES   = CHUNK_SAMPLES * (MIC_BITS / 8) * MIC_CHANNELS;

// State -----------------------------------------------------------------------
static esp_codec_dev_handle_t s_mic_dev = nullptr;
static bool s_initialized = false;
static bool s_init_failed = false;   // sticky: once init fails, do NOT retry
static bool s_codec_open = false;    // tracks esp_codec_dev_open/close pairing
static volatile bool s_running = false;
static mic_chunk_cb_t s_cb = nullptr;
static TaskHandle_t s_task = nullptr;
static int16_t *s_chunk_buf = nullptr;

// ---------------------------------------------------------------------------
static void mic_task(void *arg)
{
    (void)arg;
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
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ===========================================================================
// Public API
// ===========================================================================

bool mic_init(void)
{
    if (s_initialized) return true;
    // Once bsp_audio_init has failed (DMA NO_MEM), the BSP leaves i2s_data_if
    // NULL. Retrying bsp_audio_codec_microphone_init asserts on NULL deref.
    // Stay disabled for the rest of the session — recording continues silently.
    if (s_init_failed) return false;

    // BSP sets up I2S TX+RX channels (needed for codec clocks).
    // ESP_ERR_INVALID_STATE means a prior partial init succeeded — only safe
    // to proceed if this isn't the first call. On the FIRST call, INVALID_STATE
    // shouldn't happen; if it does, treat it as a hard failure to be safe.
    esp_err_t err = bsp_audio_init(nullptr);
    if (err != ESP_OK) {
        log_e("bsp_audio_init: %s — mic disabled for this session",
              esp_err_to_name(err));
        s_init_failed = true;
        return false;
    }

    s_mic_dev = bsp_audio_codec_microphone_init();
    if (!s_mic_dev) {
        log_e("bsp_audio_codec_microphone_init returned NULL — mic disabled");
        s_init_failed = true;
        return false;
    }

    // Chunk buffer in PSRAM
    s_chunk_buf = (int16_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_chunk_buf) {
        log_e("chunk buf alloc failed (%lu B)", (unsigned long)CHUNK_BYTES);
        s_init_failed = true;
        return false;
    }

    // Set gain to a reasonable default (0-100; 40 = moderate)
    esp_codec_dev_set_in_gain(s_mic_dev, 40.0f);

    // Open the codec NOW while internal DMA is still fresh. Deferring to
    // mic_start() meant esp_codec_dev_open was called at record time,
    // where it internally triggers i2s_channel_reconfig_std_slot; by then
    // the DMA reserve is fragmented below what the reconfig needs and
    // the alloc fails → Store-access fault on the half-reconfigured I2S
    // channel.
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = MIC_SAMPLE_RATE;
    fs.channel         = MIC_CHANNELS;
    fs.bits_per_sample = MIC_BITS;
    fs.channel_mask    = 0;
    esp_err_t oerr = esp_codec_dev_open(s_mic_dev, &fs);
    if (oerr != ESP_OK) {
        log_e("codec open at boot: %s — mic disabled for this session",
              esp_err_to_name(oerr));
        s_init_failed = true;
        return false;
    }
    s_codec_open = true;

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

    s_cb = cb;
    s_running = true;
    // 3 KB stack is the sweet spot: smaller (2 KB) caused subtle stack
    // pressure that regressed USB EP-alloc reliability on first start;
    // bigger (4 KB) failed more often on rec 2+ heap fragmentation.
    BaseType_t tc = xTaskCreatePinnedToCore(mic_task, "mic", 3072, nullptr,
                                             5, &s_task, 0);
    if (tc != pdPASS) {
        log_e("xTaskCreate(mic_task) FAILED — no audio this session");
        s_running = false;
        s_cb = nullptr;
        return false;
    }
    log_i("Mic capture started");
    return true;
}

void mic_stop(void)
{
    if (!s_running) return;
    s_running = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    s_cb = nullptr;
    log_i("Mic capture stopped");
}

uint32_t mic_sample_rate(void) { return MIC_SAMPLE_RATE; }
uint8_t  mic_channels(void)    { return MIC_CHANNELS; }
