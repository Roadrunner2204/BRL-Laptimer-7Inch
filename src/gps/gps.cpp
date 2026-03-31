/**
 * gps.cpp — Tau1201 GNSS driver
 *
 * Liest NMEA-Daten über UART1, parst sie mit TinyGPS++ und
 * schreibt das Ergebnis in g_state.gps.
 *
 * Später hier ergänzen:
 *   - Baudrate / Update-Rate per UART konfigurieren
 *   - PPS-Interrupt für präzises Lap-Timing
 *   - Start/Ziel-Linien-Erkennung
 */

#include "gps.h"
#include "../data/lap_data.h"
#include <Arduino.h>

TinyGPSPlus      gps_parser;
static HardwareSerial gps_serial(GPS_UART_PORT);

// ---------------------------------------------------------------------------
void gps_init() {
    gps_serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[GPS] Tau1201 UART2 gestartet: RX=GPIO%d TX=GPIO%d @%d baud\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // PPS-Pin konfigurieren (jeder freie GPIO geht — hier GPIO 13)
    pinMode(GPS_PPS_PIN, INPUT);
    Serial.printf("[GPS] PPS-Pin: GPIO%d\n", GPS_PPS_PIN);
}

// ---------------------------------------------------------------------------
void gps_poll() {
    // Alle verfügbaren Bytes lesen, nicht blockierend
    while (gps_serial.available()) {
        char c = (char)gps_serial.read();
        gps_parser.encode(c);
    }

    // g_state.gps aktualisieren wenn neue Daten vorliegen
    if (gps_parser.location.isUpdated() || gps_parser.speed.isUpdated()) {
        GpsData &g = g_state.gps;

        g.valid       = gps_parser.location.isValid();
        g.lat         = gps_parser.location.lat();
        g.lon         = gps_parser.location.lng();
        g.speed_kmh   = (float)gps_parser.speed.kmph();
        g.heading_deg = (float)gps_parser.course.deg();
        g.altitude_m  = (float)gps_parser.altitude.meters();
        g.satellites  = (uint8_t)gps_parser.satellites.value();
        g.hdop        = (float)gps_parser.hdop.hdop();
    }
}
