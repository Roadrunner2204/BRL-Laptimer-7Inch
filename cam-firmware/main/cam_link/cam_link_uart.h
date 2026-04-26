#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cam_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cam-side UART link.
 *
 * Mirror of main/camera_link/cam_link.{h,cpp} but inverted: this side
 * receives commands + telemetry from the laptimer and sends STATUS /
 * ACK / VIDEO_LIST back. Uses cam_link_protocol.h verbatim — keep that
 * header byte-for-byte identical to the laptimer's copy.
 *
 * Pinout on DFR1172 (free header pins, away from the C6 SDIO slot):
 *   TX = GPIO 17  →  laptimer RX (GPIO 31)
 *   RX = GPIO 18  ←  laptimer TX (GPIO 30)
 * 115200 8N1, no flow control. Final pin choice may move once the
 * board layout is finalised — adjust here, no protocol impact.
 */

#define CAM_LINK_UART_PORT  ((uart_port_t)UART_NUM_1)
#define CAM_LINK_TX_PIN     17
#define CAM_LINK_RX_PIN     18
#define CAM_LINK_BAUD       115200

/* Lifecycle */
void cam_link_uart_init(void);
void cam_link_uart_poll(void);   /* call from logic task each iteration */

/* Send replies */
bool cam_link_send_status(const CamStatusPayload *s);
bool cam_link_send_ack(uint8_t for_type);
bool cam_link_send_nack(uint8_t for_type, uint8_t reason);
bool cam_link_send_video_list(const CamVideoListEntry *entries, uint32_t count);

/* Frame handler — implemented by the application (main.c). The UART
 * driver calls this for every successfully framed + CRC-validated
 * inbound packet. Payload memory is owned by the driver and only
 * valid for the duration of the call. */
void cam_link_on_frame(uint8_t type, const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif
