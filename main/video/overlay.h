#pragma once
#include <stdint.h>

/**
 * overlay -- Data overlay renderer for video frames
 *
 * Renders lap timing data (speed, lap time, delta, RPM, etc.) as text
 * directly onto an RGB565 frame buffer using a built-in bitmap font.
 *
 * Does NOT use LVGL (runs on Core 0, LVGL is on Core 1).
 * Uses a simple fixed-width 8x16 bitmap font for maximum speed.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize overlay system (pre-render glyph atlas).
void overlay_init(void);

/// Render data overlay onto an RGB565 frame buffer.
/// Reads current data from g_state (takes mutex briefly).
/// @param buf    RGB565 pixel buffer (row-major, top-left origin)
/// @param width  frame width in pixels
/// @param height frame height in pixels
void overlay_render(uint8_t *buf, uint16_t width, uint16_t height);

#ifdef __cplusplus
}
#endif
