/**
 * video_pipeline.cpp -- JPEG decode → overlay → encode pipeline
 *
 * Uses the ESP32-P4 hardware JPEG codec for fast decode/encode.
 * The overlay is rendered in software onto the RGB565 buffer.
 */

#include "video_pipeline.h"
#include "overlay.h"
#include "avi_writer.h"
#include "../compat.h"

#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "video_pipeline";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static jpeg_decoder_handle_t s_decoder = nullptr;
static jpeg_encoder_handle_t s_encoder = nullptr;

// RGB565 double-buffers in PSRAM
static uint8_t *s_rgb_buf[2] = {nullptr, nullptr};
static int      s_rgb_idx = 0;
static uint16_t s_width = 0, s_height = 0;
static uint32_t s_rgb_size = 0;

// MJPEG output buffer in PSRAM
static uint8_t *s_jpeg_out = nullptr;
static uint32_t s_jpeg_out_cap = 0;

// Preview (shared with LVGL via volatile pointer)
static volatile const uint8_t *s_preview_ptr = nullptr;
static volatile uint16_t s_preview_w = 0, s_preview_h = 0;

static uint8_t s_quality = 80;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool video_pipeline_init(uint16_t max_width, uint16_t max_height)
{
    s_width = max_width;
    s_height = max_height;
    s_rgb_size = (uint32_t)max_width * max_height * 2;  // RGB565

    // Allocate RGB565 double buffers in PSRAM
    for (int i = 0; i < 2; i++) {
        s_rgb_buf[i] = (uint8_t *)heap_caps_malloc(s_rgb_size, MALLOC_CAP_SPIRAM);
        if (!s_rgb_buf[i]) {
            log_e("RGB buffer %d alloc failed (%lu bytes)", i, (unsigned long)s_rgb_size);
            return false;
        }
    }

    // JPEG output buffer (generous size)
    s_jpeg_out_cap = max_width * max_height;  // worst case ~= raw size
    s_jpeg_out = (uint8_t *)heap_caps_malloc(s_jpeg_out_cap, MALLOC_CAP_SPIRAM);
    if (!s_jpeg_out) {
        log_e("JPEG output buffer alloc failed");
        return false;
    }

    // Initialize HW JPEG decoder
    jpeg_decode_engine_cfg_t dec_cfg = {};
    dec_cfg.intr_priority = 0;
    dec_cfg.timeout_ms = 100;
    esp_err_t err = jpeg_new_decoder_engine(&dec_cfg, &s_decoder);
    if (err != ESP_OK) {
        log_e("JPEG decoder init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Initialize HW JPEG encoder
    jpeg_encode_engine_cfg_t enc_cfg = {};
    enc_cfg.intr_priority = 0;
    enc_cfg.timeout_ms = 100;
    err = jpeg_new_encoder_engine(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        log_e("JPEG encoder init failed: %s", esp_err_to_name(err));
        return false;
    }

    overlay_init();

    log_i("Pipeline initialized: %dx%d, RGB buf=%lu bytes x2",
          max_width, max_height, (unsigned long)s_rgb_size);
    return true;
}

void video_pipeline_process(const uint8_t *mjpeg_in, uint32_t mjpeg_size,
                            bool recording)
{
    if (!s_decoder || !mjpeg_in || mjpeg_size == 0) return;

    uint8_t *rgb = s_rgb_buf[s_rgb_idx];
    s_rgb_idx ^= 1;  // swap for next frame

    // ── Step 1: HW JPEG Decode (MJPEG → RGB565) ──
    jpeg_decode_cfg_t dec_cfg = {};
    dec_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    dec_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_decoder, &dec_cfg,
        mjpeg_in, mjpeg_size,
        rgb, s_rgb_size, &out_size);

    if (err != ESP_OK) {
        log_e("JPEG decode failed: %s", esp_err_to_name(err));
        return;
    }

    // Determine actual decoded dimensions from JPEG header
    // The HW decoder fills the buffer row-major at the JPEG's native size
    uint16_t frame_w = s_width;
    uint16_t frame_h = s_height;
    // Parse JPEG SOF0 marker for actual dimensions
    for (uint32_t i = 0; i + 8 < mjpeg_size; i++) {
        if (mjpeg_in[i] == 0xFF && mjpeg_in[i+1] == 0xC0) {
            frame_h = (mjpeg_in[i+5] << 8) | mjpeg_in[i+6];
            frame_w = (mjpeg_in[i+7] << 8) | mjpeg_in[i+8];
            break;
        }
    }

    // ── Step 2: Render data overlay ──
    if (recording) {
        overlay_render(rgb, frame_w, frame_h);
    }

    // Update preview pointer (for LVGL settings screen)
    s_preview_ptr = rgb;
    s_preview_w = frame_w;
    s_preview_h = frame_h;

    // ── Step 3: HW JPEG Encode (RGB565 → MJPEG) ──
    if (recording && s_encoder) {
        jpeg_encode_cfg_t enc_cfg = {};
        enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
        enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
        enc_cfg.quality = s_quality;
        enc_cfg.width = frame_w;
        enc_cfg.height = frame_h;

        uint32_t enc_size = 0;
        err = jpeg_encoder_process(s_encoder, &enc_cfg,
            rgb, out_size,
            s_jpeg_out, s_jpeg_out_cap, &enc_size);

        if (err != ESP_OK) {
            log_e("JPEG encode failed: %s", esp_err_to_name(err));
            return;
        }

        // ── Step 4: Write to AVI ──
        avi_writer_write_frame(s_jpeg_out, enc_size);
    }
}

const uint8_t *video_pipeline_get_preview(uint16_t *w, uint16_t *h)
{
    if (w) *w = s_preview_w;
    if (h) *h = s_preview_h;
    return (const uint8_t *)s_preview_ptr;
}

void video_pipeline_deinit(void)
{
    if (s_decoder) { jpeg_del_decoder_engine(s_decoder); s_decoder = nullptr; }
    if (s_encoder) { jpeg_del_encoder_engine(s_encoder); s_encoder = nullptr; }
    for (int i = 0; i < 2; i++) {
        if (s_rgb_buf[i]) { heap_caps_free(s_rgb_buf[i]); s_rgb_buf[i] = nullptr; }
    }
    if (s_jpeg_out) { heap_caps_free(s_jpeg_out); s_jpeg_out = nullptr; }
    s_preview_ptr = nullptr;
}

void video_pipeline_set_quality(uint8_t q)
{
    if (q < 30) q = 30;
    if (q > 95) q = 95;
    s_quality = q;
}
