#pragma once

/**
 * data_server — Simple HTTP server for session download
 *
 * Endpoints:
 *   GET /            → status JSON
 *   GET /sessions    → JSON array of session IDs on SD
 *   GET /session/{id} → full session JSON download
 *   DELETE /session/{id} → delete session from SD
 *
 * Uses ESP32 WebServer (from arduino-esp32).
 * Only active when WiFi mode is AP or STA.
 */

void data_server_start();
void data_server_stop();
bool data_server_running();
void data_server_poll();   // call in loop() when active
