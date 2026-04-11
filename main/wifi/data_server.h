#pragma once

/**
 * data_server -- HTTP session download server + captive-portal DNS stub
 *
 * Endpoints:
 *   GET /            -> status JSON
 *   GET /sessions    -> JSON array of session IDs on SD
 *   GET /session/{id} -> full session JSON download (chunked)
 *   DELETE /session/{id} -> delete session from SD
 *   GET /generate_204 -> 204 (Android captive portal)
 *
 * Uses esp_http_server (ESP-IDF).
 * Only active when WiFi mode is AP or STA.
 */

#ifdef __cplusplus
extern "C" {
#endif

void data_server_start(void);
void data_server_stop(void);
bool data_server_running(void);
void data_server_poll(void);   // call in loop -- processes DNS queries

#ifdef __cplusplus
}
#endif
