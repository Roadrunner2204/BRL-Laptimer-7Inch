#pragma once
#include <stdint.h>
#include "driver/uart.h"
#include "../data/lap_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Tau1201 GPS driver (ESP-IDF / ESP32-P4)
 *
 * Pin / UART configuration for Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.
 *
 * IMPORTANT: GPIO 14-19 are occupied by the ESP32-C6 SDIO interface
 * (esp_hosted WiFi/BT). The original ESP32-S3 wiring (RX=19, PPS=16)
 * conflicts with those pins.  Updated to use free header GPIOs:
 *   RX  = GPIO 21   (Header pin, free)
 *   TX  = GPIO 22   (Header pin, free)
 *   PPS = GPIO 20   (Header pin, free)
 *
 * Tau1201 specs:
 *   - Dual-Frequency L1 + L5, GPS/BeiDou/Galileo/GLONASS
 *   - Accuracy: < 1 m CEP  (with L5: down to ~30 cm)
 *   - Update rate: up to 10 Hz (default: 1 Hz)
 *   - Baud rate: 115200
 */

// ---- Pin / UART configuration (ESP32-P4 Waveshare 7B board) ----
#define GPS_UART_PORT   ((uart_port_t)UART_NUM_1)
#define GPS_RX_PIN      2       // UART RX -- Tau1201 TX  (Header IO2)
#define GPS_TX_PIN      3       // UART TX -- Tau1201 RX  (Header IO3)
#define GPS_PPS_PIN     4       // PPS: pulse-per-second   (Header IO4)
#define GPS_BAUD        115200

// ---- Public functions ----
void gps_init(void);
void gps_poll(void);   // call from main loop -- non-blocking

// ---- GPS date/time (populated from NMEA RMC sentence) ----
typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    bool valid;
} GpsDateTime;
GpsDateTime gps_get_datetime(void);

#ifdef __cplusplus
}
#endif
