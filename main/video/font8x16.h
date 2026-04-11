#pragma once
#include <stdint.h>

/**
 * font8x16 -- Minimal 8x16 bitmap font for video overlay
 *
 * Covers ASCII 32-127 (96 characters). Each glyph is 8 pixels wide,
 * 16 rows tall, stored as 16 bytes (1 bit per pixel, MSB = leftmost).
 *
 * Based on the classic VGA/Terminus font style.
 */

// font8x16_data[ch - 32][row] = 8-bit pixel row (MSB left)
extern const uint8_t font8x16_data[96][16];
