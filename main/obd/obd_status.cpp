/**
 * obd_status.cpp — per-FieldId freshness tracker.
 *
 * Tiny module: 256 uint32_t timestamps (1 KB total). No locking — writes
 * happen on the logic core (obd_bt poll), reads happen
 * on logic core (display picker via obd_status_is_live) and on the http
 * server task (renders JSON). Single 32-bit-aligned word writes are
 * atomic on RISC-V; readers see either the old or the new ms value, not
 * a torn one. Good enough — the freshness check has 5 s tolerance.
 */

#include "obd_status.h"
#include "../compat.h"

#include <stdio.h>
#include <string.h>

static uint32_t s_last_ms[256] = {};

void obd_status_mark_active(uint8_t field_id)
{
    s_last_ms[field_id] = (uint32_t)millis();
}

uint32_t obd_status_last_ms(uint8_t field_id)
{
    return s_last_ms[field_id];
}

bool obd_status_is_live(uint8_t field_id)
{
    uint32_t last = s_last_ms[field_id];
    if (last == 0) return false;
    uint32_t now = (uint32_t)millis();
    return (now - last) < OBD_STATUS_LIVE_WINDOW_MS;
}

size_t obd_status_render_json(char *out, size_t out_len)
{
    if (!out || out_len < 32) return 0;
    uint32_t now = (uint32_t)millis();
    int n = snprintf(out, out_len, "{\"now\":%u,\"fields\":{", (unsigned)now);
    if (n < 0 || (size_t)n >= out_len) return 0;
    bool first = true;
    for (int i = 0; i < 256; i++) {
        if (s_last_ms[i] == 0) continue;
        int w = snprintf(out + n, out_len - n, "%s\"%d\":%u",
                         first ? "" : ",", i, (unsigned)s_last_ms[i]);
        if (w < 0 || (size_t)(n + w) >= out_len - 4) {
            // Truncate gracefully — leaves room for "}}"
            break;
        }
        n += w;
        first = false;
    }
    int w = snprintf(out + n, out_len - n, "}}");
    if (w < 0 || (size_t)(n + w) >= out_len) return 0;
    n += w;
    return (size_t)n;
}
