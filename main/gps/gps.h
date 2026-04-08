#pragma once
#include <stdint.h>
#include "driver/uart.h"
#include "../data/lap_data.h"

/**
 * Tau1201 GPS driver (ESP-IDF / ESP32-P4)
 *
 * Pin / UART configuration -- kept from ESP32-S3 wiring for now;
 * may need adjustment for ESP32-P4 board pinout.
 *
 * Tau1201 specs:
 *   - Dual-Frequency L1 + L5, GPS/BeiDou/Galileo/GLONASS
 *   - Accuracy: < 1 m CEP  (with L5: down to ~30 cm)
 *   - Update rate: up to 10 Hz (default: 1 Hz)
 *   - Baud rate: 115200
 */

// ---- Pin / UART configuration ----
#define GPS_UART_PORT   ((uart_port_t)UART_NUM_1)
#define GPS_RX_PIN      19      // UART RX -- Tau1201 TX
#define GPS_TX_PIN      20      // UART TX -- Tau1201 RX (optional)
#define GPS_PPS_PIN     16      // PPS: GPIO 16
#define GPS_BAUD        115200

// ---- Public functions ----
void gps_init();
void gps_poll();   // call from main loop -- non-blocking

// ---- GPS date/time (populated from NMEA RMC sentence) ----
typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
    bool valid;
} GpsDateTime;
GpsDateTime gps_get_datetime();
