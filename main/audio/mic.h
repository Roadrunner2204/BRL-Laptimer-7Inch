#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * mic -- ES7210 microphone capture (2 mics onboard)
 *
 * Reads PCM samples via BSP I2S + ES7210 codec. A worker task pulls
 * fixed-size chunks and invokes a callback. Sample format is 16-bit
 * little-endian, mono (channels mixed down) at configured rate.
 *
 * Used by video_mgr to add audio to MJPEG AVI recordings.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mic_chunk_cb_t)(const int16_t *pcm, uint32_t samples);

/// Initialize codec + I2S. Call once at boot.
bool mic_init(void);

/// Start capture. Callback invoked from mic task with ~100 ms chunks.
bool mic_start(mic_chunk_cb_t cb);

/// Stop capture.
void mic_stop(void);

/// Get sample rate in Hz (e.g. 22050).
uint32_t mic_sample_rate(void);

/// Get channel count in stream delivered to callback (1 = mono).
uint8_t mic_channels(void);

#ifdef __cplusplus
}
#endif
