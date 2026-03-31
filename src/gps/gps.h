#pragma once

/**
 * Tau1201 GPS driver
 *
 * Hardware wiring (Waveshare ESP32-S3 7"):
 * ┌─────────────────────────────────────────────────────────┐
 * │  Tau1201 Pin  │  ESP32-S3 GPIO  │  Bemerkung           │
 * ├───────────────┼─────────────────┼──────────────────────┤
 * │  VCC          │  3.3V           │  Achtung: max 3.6V!  │
 * │  GND          │  GND            │                      │
 * │  TX           │  GPIO 16        │  UART1 RX am ESP32   │
 * │  RX           │  GPIO 15        │  UART1 TX (optional) │
 * │  PPS          │  GPIO 11        │  optional, 1Hz Puls  │
 * └───────────────┴─────────────────┴──────────────────────┘
 *
 * GPIO 15 und 16 sind am Erweiterungsstecker J3 der Waveshare 7" Platine
 * verfügbar und werden NICHT vom RGB-LCD benutzt.
 *
 * Technische Daten Tau1201:
 *   - Dual-Frequency L1 + L5, GPS/BeiDou/Galileo/GLONASS
 *   - Genauigkeit: < 1m CEP
 *   - Update-Rate: bis 10 Hz (Standard: 1 Hz)
 *   - Baudrate: 115200
 *   - NMEA 0183 Output: GGA, RMC, GSA, GSV, VTG
 */

#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include "../data/lap_data.h"

// ---- Pin / UART Konfiguration ----
#define GPS_UART_PORT  1        // ESP32 UART1
#define GPS_RX_PIN     16       // GPS TX  →  ESP32 GPIO16
#define GPS_TX_PIN     15       // GPS RX  →  ESP32 GPIO15 (nur für Config nötig)
#define GPS_PPS_PIN    11       // optional: Pulse-Per-Second
#define GPS_BAUD       115200

// ---- Öffentliche Funktionen ----
void gps_init();
void gps_poll();   // in loop() aufrufen — nicht blockierend

// Zugriff auf den TinyGPS++ Parser (für erweiterte Nutzung)
extern TinyGPSPlus gps_parser;
