#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/**
 * avi_writer -- Minimal AVI/RIFF container writer for MJPEG video
 *
 * Writes MJPEG frames into a standard AVI file that plays in VLC,
 * Windows Media Player, etc.
 *
 * Usage:
 *   avi_writer_open("/sdcard/videos/session.avi", 1280, 720, 30);
 *   for each frame:
 *     avi_writer_write_frame(jpeg_data, jpeg_size);
 *   avi_writer_close();   // finalizes header + writes index
 *
 * Safety: header is periodically flushed so partial files are still
 * playable after unexpected power loss.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Open a new AVI file for writing.
bool avi_writer_open(const char *path, uint16_t width, uint16_t height, uint8_t fps);

/// Write one MJPEG frame. Returns true on success.
bool avi_writer_write_frame(const uint8_t *jpeg_data, uint32_t jpeg_size);

/// Close the AVI file, finalize header and index. Returns true on success.
bool avi_writer_close(void);

/// Returns true if a file is currently open for writing.
bool avi_writer_is_open(void);

/// Get number of frames written so far.
uint32_t avi_writer_frame_count(void);

/// Get current file size in bytes.
uint32_t avi_writer_file_size(void);

#ifdef __cplusplus
}
#endif
