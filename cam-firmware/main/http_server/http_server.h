#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * HTTP file server.
 *
 * Endpoints (all return CORS-permissive headers so Studio web tooling
 * works from a browser context):
 *   GET /                JSON status (device id, IP, sd info)
 *   GET /videos/list     JSON array of recorded sessions
 *   GET /video/<id>      streams /sdcard/sessions/<id>/video.avi
 *   GET /telemetry/<id>  streams /sdcard/sessions/<id>/telemetry.ndjson
 *
 * The phone / Studio reaches this server via a 302 Redirect from the
 * laptimer's data_server (../../main/wifi/data_server.cpp).
 */

#define BRL_CAM_HTTP_PORT  80

void     http_server_start(void);
void     http_server_stop(void);
uint16_t http_server_port(void);

#ifdef __cplusplus
}
#endif
