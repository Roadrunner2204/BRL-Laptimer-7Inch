#pragma once

/**
 * Tau1201 GPS driver
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  Tau1201 Pin  │  Waveshare UART2-Stecker  │  Bemerkung             │
 * ├───────────────┼───────────────────────────┼────────────────────────┤
 * │  VCC (rot)    │  3.3V                     │  Achtung: max 3.6V!    │
 * │  GND (schwarz)│  GND                      │                        │
 * │  TX  (gelb)   │  RX  (GPIO 19)            │  Daten GPS → ESP32     │
 * │  RX  (grün)   │  TX  (GPIO 20)            │  Konfigbefehle (opt.)  │
 * │  PPS          │  beliebiger freier GPIO   │  → z.B. GPIO 13        │
 * └───────────────┴───────────────────────────┴────────────────────────┘
 *
 * UART1 am Board = USB-C (nicht verwenden!)
 * UART2 am Board = 4-Pin Stecker: GND / TX / RX  ← GPS hier anschließen
 *
 * PPS (Pulse Per Second):
 *   Der 1Hz-Puls ist NICHT auf GPIO 11 beschränkt — du kannst ihn an
 *   jeden freien GPIO anschließen. Empfehlung: GPIO 13 (frei, nicht vom LCD
 *   oder UART belegt). GPIO 13 → GPS_PPS_PIN ändern wenn gewünscht.
 *
 * Technische Daten Tau1201:
 *   - Dual-Frequency L1 + L5, GPS/BeiDou/Galileo/GLONASS
 *   - Genauigkeit: < 1 m CEP  (mit L5: bis ~30 cm)
 *   - Update-Rate: bis 10 Hz (Standard: 1 Hz)
 *   - Baudrate: 115200
 */

#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include "../data/lap_data.h"

// ---- Pin / UART Konfiguration ----
#define GPS_UART_PORT   2       // ESP32 UART2 = physischer UART2-Stecker
#define GPS_RX_PIN      19      // UART2-Stecker RX → Tau1201 TX
#define GPS_TX_PIN      20      // UART2-Stecker TX → Tau1201 RX (optional)
#define GPS_PPS_PIN     13      // PPS: beliebiger freier GPIO, hier GPIO 13
                                // (GPIO 11, 12, 15, 16 wären auch frei)
#define GPS_BAUD        115200

// ---- Öffentliche Funktionen ----
void gps_init();
void gps_poll();   // in loop() aufrufen — nicht blockierend

extern TinyGPSPlus gps_parser;
