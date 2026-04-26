/**
 * http_server.c — STUB. Streams videos to the phone / Studio after the
 * laptimer's data_server emits a 302 redirect to us. Real impl pending.
 */

#include "http_server.h"
#include "esp_log.h"

static const char *TAG = "http_server";

static bool s_running = false;

void http_server_start(void) {
    ESP_LOGI(TAG, "start (stub) — TODO: esp_http_server with /videos/list and /video/<id>");
    s_running = true;
    /* TODO:
     *   - httpd_start with CORS headers (mirror data_server.cpp)
     *   - GET /videos/list -> JSON of recorder_get_video_index()
     *   - GET /video/<id>  -> chunked stream of /sdcard/sessions/<id>/video.avi
     */
}

void http_server_stop(void) {
    s_running = false;
}

uint16_t http_server_port(void) { return BRL_CAM_HTTP_PORT; }
