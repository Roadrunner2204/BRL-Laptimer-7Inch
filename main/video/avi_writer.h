#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/**
 * avi_writer -- Minimal AVI/RIFF container writer for MJPEG video + PCM audio
 *
 * Writes MJPEG video (stream 0, '00dc') and optional PCM audio (stream 1,
 * '01wb') into a standard AVI file that plays in VLC, Windows Media Player,
 * etc.
 *
 * Usage:
 *   avi_writer_open("/sdcard/videos/session.avi", 1280, 720, 30,
 *                   22050, 1, 16);   // audio: 22050 Hz mono 16-bit
 *   for each video frame:  avi_writer_write_frame(jpeg, size);
 *   for each audio chunk:  avi_writer_write_audio(pcm, sample_count);
 *   avi_writer_close();   // finalizes headers + index
 *
 * Set audio_rate = 0 to disable audio stream.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Open a new AVI file.
/// audio_rate: PCM sample rate in Hz, or 0 to record video-only.
/// audio_channels: 1 or 2. bits_per_sample: 16 (currently only supported value).
bool avi_writer_open(const char *path, uint16_t width, uint16_t height, uint8_t fps,
                     uint32_t audio_rate, uint8_t audio_channels, uint8_t audio_bits);

/// Write one MJPEG frame.
bool avi_writer_write_frame(const uint8_t *jpeg_data, uint32_t jpeg_size);

/// Write audio samples (PCM 16-bit little-endian, interleaved if stereo).
/// `sample_count` is samples per channel. Returns true on success.
bool avi_writer_write_audio(const int16_t *pcm, uint32_t sample_count);

/// Close and finalize.
bool avi_writer_close(void);

bool     avi_writer_is_open(void);
uint32_t avi_writer_frame_count(void);
uint32_t avi_writer_file_size(void);

#ifdef __cplusplus
}
#endif
