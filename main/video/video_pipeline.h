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

/// Initialize pipeline (allocate buffers, init HW JPEG codec).
bool video_pipeline_init(uint16_t max_width, uint16_t max_height);

/// Process one MJPEG frame through the full pipeline.
/// If recording, writes to AVI. If preview, updates preview buffer.
/// @param mjpeg_in   input MJPEG data
/// @param mjpeg_size input size
/// @param recording  true = full pipeline with AVI write
void video_pipeline_process(const uint8_t *mjpeg_in, uint32_t mjpeg_size,
                            bool recording);

/// Get pointer to latest RGB565 preview frame.
/// Returns NULL if not available.
const uint8_t *video_pipeline_get_preview(uint16_t *w, uint16_t *h);

/// Release pipeline resources.
void video_pipeline_deinit(void);

/// Set JPEG encode quality (30-95).
void video_pipeline_set_quality(uint8_t q);

#ifdef __cplusplus
}
#endif
