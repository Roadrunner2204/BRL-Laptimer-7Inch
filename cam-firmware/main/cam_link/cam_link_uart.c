/**
 * cam_link_uart.c — cam-side UART driver for the BRL camera link.
 *
 * Same framing as the laptimer side (see cam_link_protocol.h). This
 * file is the mirror image: it RECEIVES commands + telemetry and
 * SENDS replies back. Frame dispatch is delegated to cam_link_on_frame()
 * which lives in main.c.
 */

#include "cam_link_uart.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cam_link_uart";

#define CAM_LINK_RX_BUF_SIZE   1024
#define CAM_LINK_TX_BUF_SIZE   2048

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

/* ── Internal: send one framed packet ─────────────────────────────── */
static bool send_frame(uint8_t type, const void *payload, uint16_t len)
{
    if (len > CAM_LINK_MAX_PAYLOAD) return false;

    uint8_t hdr[4];
    hdr[0] = CAM_LINK_SOF;
    hdr[1] = type;
    hdr[2] = (uint8_t)(len & 0xFF);
    hdr[3] = (uint8_t)((len >> 8) & 0xFF);

    uint8_t crc_buf[3 + CAM_LINK_MAX_PAYLOAD];
    memcpy(crc_buf, &hdr[1], 3);
    if (len > 0 && payload) memcpy(crc_buf + 3, payload, len);
    uint8_t crc = cam_link_crc8(crc_buf, 3 + len);

    int w1 = uart_tx_chars(CAM_LINK_UART_PORT, (const char *)hdr, sizeof(hdr));
    int w2 = (len > 0)
        ? uart_tx_chars(CAM_LINK_UART_PORT, (const char *)payload, len)
        : 0;
    int w3 = uart_tx_chars(CAM_LINK_UART_PORT, (const char *)&crc, 1);

    return (w1 == (int)sizeof(hdr) && w2 == (int)len && w3 == 1);
}

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
            uint8_t cb[3 + CAM_LINK_MAX_PAYLOAD];
            cb[0] = s_rx.type;
            cb[1] = (uint8_t)(s_rx.len & 0xFF);
            cb[2] = (uint8_t)((s_rx.len >> 8) & 0xFF);
            memcpy(cb + 3, s_rx.payload, s_rx.len);
            uint8_t calc = cam_link_crc8(cb, 3 + s_rx.len);
            if (calc == b) {
                cam_link_on_frame(s_rx.type, s_rx.payload, s_rx.len);
            } else {
                ESP_LOGD(TAG, "CRC fail type=0x%02X len=%u", s_rx.type, s_rx.len);
            }
            s_rx.state = RX_HUNT_SOF;
            break;
        }
        }
    }
}

/* ── Public ───────────────────────────────────────────────────────── */

void cam_link_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = CAM_LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CAM_LINK_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CAM_LINK_UART_PORT,
                                 CAM_LINK_TX_PIN, CAM_LINK_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CAM_LINK_UART_PORT,
                                        CAM_LINK_RX_BUF_SIZE * 2,
                                        CAM_LINK_TX_BUF_SIZE,
                                        0, NULL, 0));
    memset(&s_rx, 0, sizeof(s_rx));
    ESP_LOGI(TAG, "UART%d started: TX=GPIO%d RX=GPIO%d @%d baud",
             CAM_LINK_UART_PORT, CAM_LINK_TX_PIN, CAM_LINK_RX_PIN, CAM_LINK_BAUD);
}

void cam_link_uart_poll(void)
{
    uint8_t buf[CAM_LINK_RX_BUF_SIZE];
    int n = uart_read_bytes(CAM_LINK_UART_PORT, buf, sizeof(buf), 0);
    if (n > 0) rx_feed(buf, n);
}

bool cam_link_send_status(const CamStatusPayload *s) {
    return s ? send_frame(CAM_FRAME_STATUS, s, sizeof(*s)) : false;
}
bool cam_link_send_ack(uint8_t for_type) {
    return send_frame(CAM_FRAME_ACK, &for_type, 1);
}
bool cam_link_send_nack(uint8_t for_type, uint8_t reason) {
    uint8_t p[2] = { for_type, reason };
    return send_frame(CAM_FRAME_NACK, p, 2);
}
bool cam_link_send_video_list(const CamVideoListEntry *entries, uint32_t count) {
    if (count == 0) return send_frame(CAM_FRAME_VIDEO_LIST, NULL, 0);
    /* Single frame fits floor(256 / sizeof(CamVideoListEntry)) entries.
     * For larger lists the cam should send multiple VIDEO_LIST frames or
     * the laptimer can paginate via a future GET_VIDEO_LIST control. */
    uint32_t bytes = count * sizeof(CamVideoListEntry);
    if (bytes > CAM_LINK_MAX_PAYLOAD) {
        bytes = (CAM_LINK_MAX_PAYLOAD / sizeof(CamVideoListEntry))
              * sizeof(CamVideoListEntry);
    }
    return send_frame(CAM_FRAME_VIDEO_LIST, entries, (uint16_t)bytes);
}
