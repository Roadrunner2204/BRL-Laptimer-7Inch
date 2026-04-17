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

// RGB565 triple-buffers (3 slots) in PSRAM (DMA-aligned via jpeg_alloc_decoder_mem)
// Three slots allow: one being decoded, one waiting in queue, one being encoded.
static uint8_t *s_rgb_buf[VP_RGB_SLOT_COUNT] = {nullptr, nullptr, nullptr};
static uint16_t s_rgb_slot_w[VP_RGB_SLOT_COUNT] = {0};
static uint16_t s_rgb_slot_h[VP_RGB_SLOT_COUNT] = {0};
static int      s_rgb_idx = 0;
static uint16_t s_width = 0, s_height = 0;
static uint32_t s_rgb_size = 0;
static uint32_t s_rgb_alloc_size = 0;  // actual aligned allocation size

// MJPEG output buffer in PSRAM (DMA-aligned via jpeg_alloc_encoder_mem)
static uint8_t *s_jpeg_out = nullptr;
static uint32_t s_jpeg_out_cap = 0;
static uint32_t s_jpeg_out_alloc = 0;  // actual aligned allocation size

// Preview (shared with LVGL via volatile pointer)
static volatile const uint8_t *s_preview_ptr = nullptr;
static volatile uint16_t s_preview_w = 0, s_preview_h = 0;

static uint8_t s_quality = 80;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void video_pipeline_early_init(void)
{
    if (s_decoder) return;  // already done

    log_i("Early JPEG codec init (before display BSP)");
    log_i("Free DMA: %lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    // Initialize HW JPEG decoder — needs contiguous DMA RAM
    // Timeout is generous: at 1080p with cache flush overhead, 100ms can miss.
    jpeg_decode_engine_cfg_t dec_cfg = {};
    dec_cfg.intr_priority = 0;
    dec_cfg.timeout_ms = 200;
    esp_err_t err = jpeg_new_decoder_engine(&dec_cfg, &s_decoder);
    if (err != ESP_OK) {
        log_e("JPEG decoder init failed: %s", esp_err_to_name(err));
        return;
    }

    // Initialize HW JPEG encoder
    jpeg_encode_engine_cfg_t enc_cfg = {};
    enc_cfg.intr_priority = 0;
    enc_cfg.timeout_ms = 200;
    err = jpeg_new_encoder_engine(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        log_e("JPEG encoder init failed: %s", esp_err_to_name(err));
        return;
    }

    log_i("JPEG codec ready (free DMA: %lu)",
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

bool video_pipeline_init(uint16_t max_width, uint16_t max_height)
{
    // Free any previous buffers (resolution change) — keep codec engines alive
    for (int i = 0; i < 2; i++) {
        if (s_rgb_buf[i]) { heap_caps_free(s_rgb_buf[i]); s_rgb_buf[i] = nullptr; }
    }
    if (s_jpeg_out) { heap_caps_free(s_jpeg_out); s_jpeg_out = nullptr; }
    s_preview_ptr = nullptr;

    s_width = max_width;
    s_height = max_height;
    s_rgb_size = (uint32_t)max_width * max_height * 2;  // RGB565

    // Allocate RGB565 triple buffers — DMA-aligned for HW JPEG decoder
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {};
    mem_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;

    for (int i = 0; i < VP_RGB_SLOT_COUNT; i++) {
        size_t actual = 0;
        s_rgb_buf[i] = (uint8_t *)jpeg_alloc_decoder_mem(s_rgb_size, &mem_cfg, &actual);
        if (!s_rgb_buf[i]) {
            log_e("RGB buffer %d alloc failed (%lu bytes)", i, (unsigned long)s_rgb_size);
            return false;
        }
        if (i == 0) s_rgb_alloc_size = (uint32_t)actual;
        s_rgb_slot_w[i] = 0;
        s_rgb_slot_h[i] = 0;
    }

    // JPEG output buffer — DMA-aligned for HW JPEG encoder
    s_jpeg_out_cap = max_width * max_height;  // worst case ~= raw size
    jpeg_encode_memory_alloc_cfg_t enc_mem_cfg = {};
    enc_mem_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    size_t enc_actual = 0;
    s_jpeg_out = (uint8_t *)jpeg_alloc_encoder_mem(s_jpeg_out_cap, &enc_mem_cfg, &enc_actual);
    if (!s_jpeg_out) {
        log_e("JPEG output buffer alloc failed");
        return false;
    }
    s_jpeg_out_alloc = (uint32_t)enc_actual;

    // Init JPEG codec if not already done by early_init
    if (!s_decoder || !s_encoder) {
        video_pipeline_early_init();
        if (!s_decoder || !s_encoder) {
            log_e("JPEG codec not available");
            return false;
        }
    }

    overlay_init();

    log_i("Pipeline initialized: %dx%d, RGB buf=%lu bytes x2",
          max_width, max_height, (unsigned long)s_rgb_size);
    return true;
}

// Helper: verify a buffer looks like a valid JPEG (SOI + EOI present).
// UVC can deliver partial frames when ISO packets get dropped.
static bool jpeg_sanity_ok(const uint8_t *data, uint32_t size)
{
    if (!data || size < 10) return false;
    if (data[0] != 0xFF || data[1] != 0xD8) return false;         // SOI
    if (data[size-2] != 0xFF || data[size-1] != 0xD9) return false; // EOI
    return true;
}

// Recreate the HW decoder engine — used to recover after a stuck transaction.
static void reset_decoder(void)
{
    if (s_decoder) { jpeg_del_decoder_engine(s_decoder); s_decoder = nullptr; }
    jpeg_decode_engine_cfg_t cfg = {};
    cfg.intr_priority = 0;
    cfg.timeout_ms = 200;
    jpeg_new_decoder_engine(&cfg, &s_decoder);
    log_w("JPEG decoder reset");
}

void video_pipeline_process(const uint8_t *mjpeg_in, uint32_t mjpeg_size,
                            bool recording)
{
    if (!s_decoder || !mjpeg_in || mjpeg_size == 0) return;

    // Guard: don't feed incomplete JPEGs to the HW decoder. A bad frame
    // leaves the decoder in a stuck state that cascades to every following
    // frame ("transaction not in-flight" / "EOI marker not read").
    if (!jpeg_sanity_ok(mjpeg_in, mjpeg_size)) return;

    uint8_t *rgb = s_rgb_buf[s_rgb_idx];
    s_rgb_idx ^= 1;  // swap for next frame

    // ── Step 1: HW JPEG Decode (MJPEG → RGB565) ──
    jpeg_decode_cfg_t dec_cfg = {};
    dec_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    dec_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_decoder, &dec_cfg,
        mjpeg_in, mjpeg_size,
        rgb, s_rgb_alloc_size, &out_size);

    if (err != ESP_OK) {
        log_e("JPEG decode failed: %s", esp_err_to_name(err));
        // TIMEOUT / INVALID_STATE: the decoder engine is stuck. Recreate it
        // so the next frame has a clean shot.
        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
            reset_decoder();
        }
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
        enc_cfg.image_quality = s_quality;
        enc_cfg.width = frame_w;
        enc_cfg.height = frame_h;

        uint32_t enc_size = 0;
        err = jpeg_encoder_process(s_encoder, &enc_cfg,
            rgb, out_size,
            s_jpeg_out, s_jpeg_out_alloc, &enc_size);

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
    for (int i = 0; i < VP_RGB_SLOT_COUNT; i++) {
        if (s_rgb_buf[i]) { heap_caps_free(s_rgb_buf[i]); s_rgb_buf[i] = nullptr; }
    }
    if (s_jpeg_out) { heap_caps_free(s_jpeg_out); s_jpeg_out = nullptr; }
    s_preview_ptr = nullptr;
}

// ──────────────────────────────────────────────────────────────────────────
// Split-stage API: decode into specified slot, encode from specified slot.
// Enables 2-task parallel pipeline: decoder engine runs ahead of encoder.
// ──────────────────────────────────────────────────────────────────────────

bool video_pipeline_decode_into(int slot,
                                const uint8_t *mjpeg_in, uint32_t mjpeg_size,
                                uint16_t *w_out, uint16_t *h_out,
                                bool render_overlay)
{
    if (!s_decoder || !mjpeg_in || mjpeg_size == 0) return false;
    if (slot < 0 || slot >= VP_RGB_SLOT_COUNT || !s_rgb_buf[slot]) return false;

    // Sanity check: reject partial JPEGs (SOI + EOI must be present)
    if (mjpeg_size < 10) return false;
    if (mjpeg_in[0] != 0xFF || mjpeg_in[1] != 0xD8) return false;
    if (mjpeg_in[mjpeg_size-2] != 0xFF || mjpeg_in[mjpeg_size-1] != 0xD9) return false;

    uint8_t *rgb = s_rgb_buf[slot];

    jpeg_decode_cfg_t dec_cfg = {};
    dec_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    dec_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_decoder, &dec_cfg,
        mjpeg_in, mjpeg_size,
        rgb, s_rgb_alloc_size, &out_size);
    if (err != ESP_OK) {
        log_e("decode_into slot=%d: %s", slot, esp_err_to_name(err));
        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
            reset_decoder();
        }
        return false;
    }

    // Extract actual dimensions from SOF0
    uint16_t fw = s_width, fh = s_height;
    for (uint32_t i = 0; i + 8 < mjpeg_size; i++) {
        if (mjpeg_in[i] == 0xFF && mjpeg_in[i+1] == 0xC0) {
            fh = (mjpeg_in[i+5] << 8) | mjpeg_in[i+6];
            fw = (mjpeg_in[i+7] << 8) | mjpeg_in[i+8];
            break;
        }
    }

    if (render_overlay) overlay_render(rgb, fw, fh);

    s_rgb_slot_w[slot] = fw;
    s_rgb_slot_h[slot] = fh;

    // Keep preview path pointing at the freshest frame
    s_preview_ptr = rgb;
    s_preview_w = fw;
    s_preview_h = fh;

    if (w_out) *w_out = fw;
    if (h_out) *h_out = fh;
    return true;
}

bool video_pipeline_encode_from(int slot, uint16_t w, uint16_t h)
{
    if (!s_encoder || slot < 0 || slot >= VP_RGB_SLOT_COUNT) return false;
    if (!s_rgb_buf[slot] || !s_jpeg_out) return false;
    if (w == 0 || h == 0) return false;

    jpeg_encode_cfg_t enc_cfg = {};
    enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    enc_cfg.image_quality = s_quality;
    enc_cfg.width = w;
    enc_cfg.height = h;

    uint32_t enc_size = 0;
    esp_err_t err = jpeg_encoder_process(s_encoder, &enc_cfg,
        s_rgb_buf[slot], (uint32_t)w * h * 2,
        s_jpeg_out, s_jpeg_out_alloc, &enc_size);
    if (err != ESP_OK) {
        log_e("encode_from slot=%d: %s", slot, esp_err_to_name(err));
        return false;
    }
    avi_writer_write_frame(s_jpeg_out, enc_size);
    return true;
}

const uint8_t *video_pipeline_get_rgb_slot(int slot, uint16_t *w, uint16_t *h)
{
    if (slot < 0 || slot >= VP_RGB_SLOT_COUNT) return nullptr;
    if (w) *w = s_rgb_slot_w[slot];
    if (h) *h = s_rgb_slot_h[slot];
    return s_rgb_buf[slot];
}

void video_pipeline_set_quality(uint8_t q)
{
    if (q < 30) q = 30;
    if (q > 95) q = 95;
    s_quality = q;
}

// No standalone decode - LVGL handles MJPEG directly via lv_image_decoder
