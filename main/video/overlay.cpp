/**
 * overlay.cpp -- Data overlay renderer for video frames
 *
 * Renders a semi-transparent dark bar at the bottom of the frame with
 * lap timing data in white text using a minimal built-in bitmap font.
 */

#include "overlay.h"
#include "../data/lap_data.h"
#include "../compat.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "overlay";

// ---------------------------------------------------------------------------
// Minimal 8x16 bitmap font (ASCII 32-127, 1-bit-per-pixel)
// Each character is 8 pixels wide, 16 pixels tall = 16 bytes per glyph.
// This is a simplified Terminus/VGA-style font embedded as a C array.
// ---------------------------------------------------------------------------

// We generate a very simple font: only digits, letters, and common symbols.
// For space efficiency, we store only the printable ASCII range (32-127).

#include "font8x16.h"   // extern const uint8_t font8x16_data[96][16];

// ---------------------------------------------------------------------------
// RGB565 helpers
// ---------------------------------------------------------------------------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static inline uint16_t blend_rgb565(uint16_t bg, uint16_t fg, uint8_t alpha) {
    // Fast alpha blend: alpha is 0-255
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;

    uint8_t inv = 255 - alpha;
    uint16_t r = ((((fg >> 11) & 0x1F) * alpha + ((bg >> 11) & 0x1F) * inv) >> 8);
    uint16_t g = ((((fg >> 5) & 0x3F) * alpha + ((bg >> 5) & 0x3F) * inv) >> 8);
    uint16_t b = (((fg & 0x1F) * alpha + (bg & 0x1F) * inv) >> 8);
    return (r << 11) | (g << 5) | b;
}

// ---------------------------------------------------------------------------
// Draw a single character at (x, y) onto the RGB565 buffer
// ---------------------------------------------------------------------------
static void draw_char(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                      int x, int y, char c, uint16_t color)
{
    if (c < 32 || c > 127) c = '?';
    const uint8_t *glyph = font8x16_data[c - 32];

    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < 0 || py >= fb_h) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= fb_w) continue;
            if (bits & (0x80 >> col)) {
                fb[py * fb_w + px] = color;
            }
        }
    }
}

// Draw a string at (x, y). Returns the x position after the last char.
static int draw_string(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                       int x, int y, const char *str, uint16_t color)
{
    while (*str) {
        draw_char(fb, fb_w, fb_h, x, y, *str, color);
        x += 8;
        str++;
    }
    return x;
}

// Draw a filled rectangle
static void fill_rect(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                      int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    for (int row = y; row < y + h && row < fb_h; row++) {
        if (row < 0) continue;
        for (int col = x; col < x + w && col < fb_w; col++) {
            if (col < 0) continue;
            fb[row * fb_w + col] = blend_rgb565(fb[row * fb_w + col], color, alpha);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void overlay_init(void)
{
    log_i("Overlay renderer initialized (8x16 bitmap font)");
}

void overlay_render(uint8_t *buf, uint16_t width, uint16_t height)
{
    if (!buf || width == 0 || height == 0) return;

    uint16_t *fb = (uint16_t *)buf;

    // Snapshot g_state under mutex
    float speed_kmh = 0;
    uint32_t lap_ms = 0;
    uint32_t best_ms = 0;
    int32_t delta_ms = 0;
    float rpm = 0;
    bool timing_active = false;
    uint8_t lap_count = 0;
    uint8_t current_sector = 0;
    uint32_t sector_ms[MAX_SECTORS] = {};

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    {
        speed_kmh = g_state.gps.speed_kmh;
        timing_active = g_state.timing.timing_active;
        delta_ms = g_state.timing.live_delta_ms;
        rpm = g_state.obd.rpm;
        lap_count = g_state.session.lap_count;
        current_sector = g_state.timing.current_sector;

        if (timing_active) {
            lap_ms = millis() - g_state.timing.lap_start_ms;
        } else if (lap_count > 0) {
            lap_ms = g_state.session.laps[lap_count - 1].total_ms;
        }

        if (lap_count > 0) {
            best_ms = g_state.session.laps[g_state.session.best_lap_idx].total_ms;
        }

        // Copy sector times of current/last lap
        if (lap_count > 0) {
            const RecordedLap &rl = g_state.session.laps[
                timing_active ? lap_count : lap_count - 1];
            for (int i = 0; i < MAX_SECTORS; i++) {
                sector_ms[i] = rl.sector_ms[i];
            }
        }
    }
    xSemaphoreGive(g_state_mutex);

    // ── Draw overlay background (semi-transparent dark bar, bottom 40px) ──
    const int bar_h = 40;
    const int bar_y = height - bar_h;
    const uint16_t bg_color = rgb565(0, 0, 0);
    fill_rect(fb, width, height, 0, bar_y, width, bar_h, bg_color, 180);

    // ── Overlay colors ──
    const uint16_t white  = rgb565(255, 255, 255);
    const uint16_t green  = rgb565(0, 204, 102);
    const uint16_t red    = rgb565(255, 68, 68);
    const uint16_t yellow = rgb565(255, 200, 0);
    const uint16_t gray   = rgb565(160, 160, 160);

    char txt[32];
    int x = 8;
    int y1 = bar_y + 4;    // top row
    int y2 = bar_y + 22;   // bottom row

    // ── Top row: Speed | Lap Time | Best Lap | Delta ──

    // Speed
    snprintf(txt, sizeof(txt), "%.0f km/h", (double)speed_kmh);
    x = draw_string(fb, width, height, x, y1, txt, white);
    x += 24;

    // Lap time
    snprintf(txt, sizeof(txt), "LAP %u:%05.2f",
             (unsigned)(lap_ms / 60000), fmod(lap_ms / 1000.0, 60.0));
    x = draw_string(fb, width, height, x, y1, txt, timing_active ? white : gray);
    x += 24;

    // Best lap
    if (best_ms > 0) {
        snprintf(txt, sizeof(txt), "BEST %u:%05.2f",
                 (unsigned)(best_ms / 60000), fmod(best_ms / 1000.0, 60.0));
    } else {
        snprintf(txt, sizeof(txt), "BEST ---");
    }
    x = draw_string(fb, width, height, x, y1, txt, yellow);
    x += 24;

    // Delta
    if (lap_count > 0 && timing_active) {
        snprintf(txt, sizeof(txt), "%+.2f s", delta_ms / 1000.0);
        uint16_t dcol = delta_ms < 0 ? green : (delta_ms > 0 ? red : white);
        draw_string(fb, width, height, x, y1, txt, dcol);
    }

    // ── Bottom row: RPM | Sector | G-Force ──
    x = 8;

    // RPM
    if (rpm > 0) {
        snprintf(txt, sizeof(txt), "RPM %.0f", (double)rpm);
    } else {
        snprintf(txt, sizeof(txt), "RPM ---");
    }
    x = draw_string(fb, width, height, x, y2, txt, white);
    x += 24;

    // Current sector
    if (timing_active && current_sector > 0) {
        uint32_t sec_ms = sector_ms[current_sector - 1];
        snprintf(txt, sizeof(txt), "S%d %.1f", current_sector, sec_ms / 1000.0);
        x = draw_string(fb, width, height, x, y2, txt, gray);
        x += 24;
    }

    // G-force (from GPS — lateral and longitudinal approximation)
    // Simple: use speed change as longitudinal G, heading change as lateral G
    // For now, show placeholder until accelerometer is available
    snprintf(txt, sizeof(txt), "G --/--");
    draw_string(fb, width, height, x, y2, txt, gray);
}
