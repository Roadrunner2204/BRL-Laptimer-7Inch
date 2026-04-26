/**
 * cam_link.cpp — main-side driver for the external BRL camera module.
 *
 * Pure transport layer: framing, CRC, UART RX/TX. No ties to lap_data.h
 * or g_state — telemetry callers (gps_poll, obd_poll, lap_timer) push
 * already-built CamTelemetry* structs in. Keeps this module testable
 * standalone and lets the cam-firmware repo reuse cam_link_protocol.h
 * verbatim.
 */

#include "cam_link.h"
#include "../compat.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "cam_link";

/* ── UART buffers ─────────────────────────────────────────────────── */
#define CAM_LINK_RX_BUF_SIZE   1024
#define CAM_LINK_TX_BUF_SIZE   2048   /* must hold ~10 telemetry frames */

/* RX framer state */
typedef enum {
    RX_HUNT_SOF = 0,
    RX_TYPE,
    RX_LEN_LO,
    RX_LEN_HI,
    RX_PAYLOAD,
    RX_CRC,
} RxState;

static struct {
    RxState  state;
    uint8_t  type;
    uint16_t len;
    uint16_t got;
    uint8_t  payload[CAM_LINK_MAX_PAYLOAD];
} s_rx;

/* Cached link status */
static CamLinkInfo s_info = {};

/* Cached video index from the cam */
static CamVideoIndex s_video_idx = {};
static uint64_t      s_video_idx_requested_ms = 0;

/* ── Internal: send one framed packet ─────────────────────────────── */
static bool send_frame(uint8_t type, const void *payload, uint16_t len)
{
    if (len > CAM_LINK_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "send_frame: payload too large (%u > %u)",
                 (unsigned)len, (unsigned)CAM_LINK_MAX_PAYLOAD);
        return false;
    }

    /* Build header (SOF | TYPE | LEN-LE) and compute CRC over
     * TYPE | LEN | PAYLOAD as a single contiguous buffer. */
    uint8_t hdr[4];
    hdr[0] = CAM_LINK_SOF;
    hdr[1] = type;
    hdr[2] = (uint8_t)(len & 0xFF);
    hdr[3] = (uint8_t)((len >> 8) & 0xFF);

    uint8_t crc_buf[3 + CAM_LINK_MAX_PAYLOAD];
    memcpy(crc_buf, &hdr[1], 3);
    if (len > 0 && payload) {
        memcpy(crc_buf + 3, payload, len);
    }
    uint8_t crc = cam_link_crc8(crc_buf, 3 + len);

    /* Non-blocking write — drop frame if TX buffer is full to avoid
     * stalling the logic task (telemetry is best-effort). */
    int w1 = uart_tx_chars(CAM_LINK_UART_PORT, (const char *)hdr, sizeof(hdr));
    int w2 = (len > 0)
        ? uart_tx_chars(CAM_LINK_UART_PORT, (const char *)payload, len)
        : 0;
    int w3 = uart_tx_chars(CAM_LINK_UART_PORT, (const char *)&crc, 1);

    if (w1 < (int)sizeof(hdr) || w2 < (int)len || w3 < 1) {
        ESP_LOGD(TAG, "tx drop: type=0x%02X len=%u (queue full)",
                 type, (unsigned)len);
        return false;
    }
    return true;
}

/* ── Internal: handle a complete frame from the cam ───────────────── */
static void handle_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    switch (type) {
    case CAM_FRAME_STATUS:
        if (len == sizeof(CamStatusPayload)) {
            memcpy(&s_info.status, payload, sizeof(CamStatusPayload));
            s_info.last_status_ms = esp_timer_get_time() / 1000;
            s_info.link_up = true;
        }
        break;

    case CAM_FRAME_VIDEO_LIST: {
        uint32_t entry_sz = sizeof(CamVideoListEntry);
        if (len % entry_sz != 0) {
            ESP_LOGW(TAG, "VIDEO_LIST: bad len %u (entry=%u)",
                     (unsigned)len, (unsigned)entry_sz);
            break;
        }
        uint32_t n = len / entry_sz;
        CamVideoListEntry *fresh = NULL;
        if (n > 0) {
            fresh = (CamVideoListEntry *)malloc(len);
            if (!fresh) {
                ESP_LOGE(TAG, "VIDEO_LIST: malloc(%u) failed", (unsigned)len);
                break;
            }
            memcpy(fresh, payload, len);
        }
        free(s_video_idx.entries);
        s_video_idx.entries = fresh;
        s_video_idx.count   = n;
        s_video_idx.refreshed_ms = esp_timer_get_time() / 1000;
        break;
    }

    case CAM_FRAME_ACK:
    case CAM_FRAME_NACK:
        /* Logged for diagnostics; UI uses STATUS for true REC state. */
        ESP_LOGD(TAG, "%s for type 0x%02X",
                 type == CAM_FRAME_ACK ? "ACK" : "NACK",
                 (unsigned)payload[0]);
        break;

    default:
        ESP_LOGW(TAG, "unknown frame type 0x%02X len %u",
                 (unsigned)type, (unsigned)len);
        break;
    }
}

/* ── Internal: feed RX bytes through the framer ───────────────────── */
static void rx_feed(const uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t b = buf[i];
        switch (s_rx.state) {
        case RX_HUNT_SOF:
            if (b == CAM_LINK_SOF) s_rx.state = RX_TYPE;
            break;
        case RX_TYPE:
            s_rx.type = b;
            s_rx.state = RX_LEN_LO;
            break;
        case RX_LEN_LO:
            s_rx.len = b;
            s_rx.state = RX_LEN_HI;
            break;
        case RX_LEN_HI:
            s_rx.len |= ((uint16_t)b) << 8;
            if (s_rx.len > CAM_LINK_MAX_PAYLOAD) {
                /* Bad frame — resync */
                s_rx.state = RX_HUNT_SOF;
                break;
            }
            s_rx.got = 0;
            s_rx.state = (s_rx.len > 0) ? RX_PAYLOAD : RX_CRC;
            break;
        case RX_PAYLOAD:
            s_rx.payload[s_rx.got++] = b;
            if (s_rx.got >= s_rx.len) s_rx.state = RX_CRC;
            break;
        case RX_CRC: {
            uint8_t hdr_pl[3 + CAM_LINK_MAX_PAYLOAD];
            hdr_pl[0] = s_rx.type;
            hdr_pl[1] = (uint8_t)(s_rx.len & 0xFF);
            hdr_pl[2] = (uint8_t)((s_rx.len >> 8) & 0xFF);
            memcpy(hdr_pl + 3, s_rx.payload, s_rx.len);
            uint8_t calc = cam_link_crc8(hdr_pl, 3 + s_rx.len);
            if (calc == b) {
                handle_frame(s_rx.type, s_rx.payload, s_rx.len);
            } else {
                ESP_LOGD(TAG, "CRC fail type=0x%02X len=%u (got 0x%02X want 0x%02X)",
                         s_rx.type, s_rx.len, b, calc);
            }
            s_rx.state = RX_HUNT_SOF;
            break;
        }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void cam_link_init(void)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate  = CAM_LINK_BAUD;
    uart_config.data_bits  = UART_DATA_8_BITS;
    uart_config.parity     = UART_PARITY_DISABLE;
    uart_config.stop_bits  = UART_STOP_BITS_1;
    uart_config.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(CAM_LINK_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CAM_LINK_UART_PORT,
                                 CAM_LINK_TX_PIN, CAM_LINK_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CAM_LINK_UART_PORT,
                                        CAM_LINK_RX_BUF_SIZE * 2,
                                        CAM_LINK_TX_BUF_SIZE,
                                        0, nullptr, 0));

    memset(&s_rx, 0, sizeof(s_rx));
    memset(&s_info, 0, sizeof(s_info));
    memset(&s_video_idx, 0, sizeof(s_video_idx));

    ESP_LOGI(TAG, "cam_link UART%d started: TX=GPIO%d RX=GPIO%d @%d baud",
             CAM_LINK_UART_PORT, CAM_LINK_TX_PIN, CAM_LINK_RX_PIN, CAM_LINK_BAUD);
}

void cam_link_poll(void)
{
    /* Drain RX bytes from the UART driver — non-blocking. */
    uint8_t buf[CAM_LINK_RX_BUF_SIZE];
    int n = uart_read_bytes(CAM_LINK_UART_PORT, buf, sizeof(buf), 0);
    if (n > 0) rx_feed(buf, n);

    /* Mark link as down if no STATUS frame for 2 s. */
    uint64_t now_ms = esp_timer_get_time() / 1000;
    if (s_info.link_up && (now_ms - s_info.last_status_ms) > 2000) {
        s_info.link_up = false;
        ESP_LOGW(TAG, "link timeout (no STATUS for >2 s)");
    }
}

bool cam_link_rec_start(const char *session_id,
                        uint64_t   gps_utc_ms,
                        int32_t    track_idx,
                        const char *track_name,
                        const char *car_name)
{
    CamRecStartPayload p = {};
    if (session_id) strncpy(p.session_id, session_id, sizeof(p.session_id) - 1);
    p.gps_utc_ms = gps_utc_ms;
    p.track_idx  = track_idx;
    if (track_name) strncpy(p.track_name, track_name, sizeof(p.track_name) - 1);
    if (car_name)   strncpy(p.car_name,   car_name,   sizeof(p.car_name)   - 1);
    p.proto_ver  = CAM_LINK_PROTO_VER;
    return send_frame(CAM_FRAME_REC_START, &p, sizeof(p));
}

bool cam_link_rec_stop(void)
{
    return send_frame(CAM_FRAME_REC_STOP, nullptr, 0);
}

bool cam_link_ping(void)
{
    return send_frame(CAM_FRAME_PING, nullptr, 0);
}

bool cam_link_send_gps(const CamTelemetryGps *t)
{
    return t ? send_frame(CAM_FRAME_TELE_GPS, t, sizeof(*t)) : false;
}

bool cam_link_send_obd(const CamTelemetryObd *t)
{
    return t ? send_frame(CAM_FRAME_TELE_OBD, t, sizeof(*t)) : false;
}

bool cam_link_send_analog(const CamTelemetryAnalog *t)
{
    return t ? send_frame(CAM_FRAME_TELE_ANALOG, t, sizeof(*t)) : false;
}

bool cam_link_send_lap_marker(const CamLapMarker *m)
{
    return m ? send_frame(CAM_FRAME_LAP_MARKER, m, sizeof(*m)) : false;
}

CamLinkInfo cam_link_get_info(void)        { return s_info; }
bool        cam_link_is_recording(void)    { return s_info.link_up && s_info.status.rec_active; }
CamVideoIndex cam_link_get_video_index(void) { return s_video_idx; }
