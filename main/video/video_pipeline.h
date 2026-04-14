#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * video_pipeline -- JPEG decode → overlay → encode pipeline
 *
 * Processes MJPEG frames from the UVC camera:
 * 1. HW JPEG decode to RGB565 in PSRAM
 * 2. Software overlay render (speed, laptime, delta, etc.)
 * 3. HW JPEG encode back to MJPEG
 * 4. Queue encoded frame for AVI writer
 *
 * When in preview mode, step 3-4 are skipped and the RGB565 buffer
 * is made available for LVGL display.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Early init: allocate HW JPEG codec before display BSP fragments DMA RAM.
/// Call from app_main BEFORE bsp_display_start.
void video_pipeline_early_init(void);

/// Initialize pipeline (allocate buffers). Codec must be pre-initialized.
bool video_pipeline_init(uint16_t max_width, uint16_t max_height);

/// Process one MJPEG frame through the full pipeline (legacy single-task).
void video_pipeline_process(const uint8_t *mjpeg_in, uint32_t mjpeg_size,
                            bool recording);

// ── Split-stage API (two-task pipeline: decoder and encoder in parallel) ──

/// Number of RGB slots available (for triple-buffering).
#define VP_RGB_SLOT_COUNT 3

/// Decode one MJPEG frame into RGB slot `slot`. Optionally renders overlay.
/// Returns true on success, false on decoder error. Sets *w / *h to the
/// actual decoded dimensions. The RGB565 buffer in slot is updated.
bool video_pipeline_decode_into(int slot,
                                const uint8_t *mjpeg_in, uint32_t mjpeg_size,
                                uint16_t *w, uint16_t *h,
                                bool render_overlay);

/// Encode RGB slot `slot` and write the resulting MJPEG frame to the AVI.
bool video_pipeline_encode_from(int slot, uint16_t w, uint16_t h);

/// Get pointer to RGB slot (for preview display). Returns NULL on invalid.
const uint8_t *video_pipeline_get_rgb_slot(int slot, uint16_t *w, uint16_t *h);

/// Get pointer to latest RGB565 preview frame (legacy single-buffer path).
const uint8_t *video_pipeline_get_preview(uint16_t *w, uint16_t *h);

/// Release pipeline resources.
void video_pipeline_deinit(void);

/// Set JPEG encode quality (30-95).
void video_pipeline_set_quality(uint8_t q);

#ifdef __cplusplus
}
#endif
