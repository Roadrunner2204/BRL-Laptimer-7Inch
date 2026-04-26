#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP server — exposes:
 *   GET /videos/list      JSON index of recorded sessions
 *   GET /video/<id>       streams /sdcard/sessions/<id>/video.avi
 *
 * The phone/Studio reaches this server via a 302 redirect from the
 * laptimer's data_server (see ../../main/wifi/data_server.cpp).
 *
 * Default port 80; reported back in the STATUS frame so the laptimer
 * can build the right Location: header. */

#define BRL_CAM_HTTP_PORT  80

void http_server_start(void);
void http_server_stop(void);
uint16_t http_server_port(void);

#ifdef __cplusplus
}
#endif
