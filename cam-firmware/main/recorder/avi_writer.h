#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * avi_writer — Minimal AVI/RIFF container writer for MJPEG video.
 *
 * Recycled from BRL-Laptimer commit e08cb92 with the audio paths and
 * throughput-debug snapshots removed (cam module has no microphone).
 *
 * Uses native FatFS (ff.h) directly instead of stdio, pre-allocates
 * 500 MB of contiguous clusters with f_expand() so streaming writes
 * don't touch the FAT per frame, and stages writes through a
 * cache-line-aligned internal-DMA chunk buffer so SDMMC DMA can read
 * straight out of it. These three together lifted writes from
 * ~500 KB/s (stdio path) to multi-MB/s when the cam was still on the
 * laptimer; on its own SDIO bus with no USB-ISO contention the headroom
 * is even larger.
 *
 * File layout:
 *   RIFF 'AVI '
 *     LIST 'hdrl'
 *       'avih'   MainAVIHeader (dwStreams = 1)
 *       LIST 'strl'                                    (stream 0: video)
 *         'strh'   AVIStreamHeader (vids/MJPG)
 *         'strf'   BITMAPINFOHEADER
 *     LIST 'movi'
 *       '00dc' [size] [jpeg data]                       (video frame)
 *       ...
 *     'idx1' [index entries]
 *
 * Usage:
 *   avi_writer_preallocate();                       // once at boot
 *   avi_writer_open("/sdcard/sessions/<id>/video.avi", 1920, 1080, 30);
 *   for each MJPEG frame: avi_writer_write_frame(jpeg, size);
 *   avi_writer_close();
 */

/// Pre-allocate the internal-DMA chunk buffer. Call once at boot —
/// internal-DMA RAM is tightest at startup; lazy alloc later may fail.
/// Idempotent.
bool avi_writer_preallocate(void);

bool avi_writer_open(const char *path, uint16_t width, uint16_t height, uint8_t fps);
bool avi_writer_write_frame(const uint8_t *jpeg_data, uint32_t jpeg_size);
bool avi_writer_close(void);

bool     avi_writer_is_open(void);
uint32_t avi_writer_frame_count(void);
uint32_t avi_writer_file_size(void);

#ifdef __cplusplus
}
#endif
