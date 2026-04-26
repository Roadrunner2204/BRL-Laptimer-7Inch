/**
 * cam_preview.cpp — HTTP fetch + HW JPEG decode + LVGL canvas paint.
 *
 * Pipeline (5 Hz):
 *   esp_http_client GET http://<cam-ip>:80/preview.jpg
 *   → JPEG buffer in PSRAM
 *   → jpeg_decoder_process → RGB565 buffer in PSRAM
 *   → lv_canvas_set_buffer + lv_obj_invalidate (under bsp_display_lock)
 *
 * The decoder is created once per preview session and torn down on
 * cam_preview_close() so we release the internal-DMA RAM when the
 * Video Settings screen is closed — the screen is rarely visited and
 * we don't want to hold the JPEG engine open between paddock visits.
 */

#include "cam_preview.h"
#include "../camera_link/cam_link.h"
#include "../compat.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "driver/jpeg_decode.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "bsp/esp-bsp.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "cam_preview";

#define PREVIEW_FETCH_BUF_SIZE   (256 * 1024)   /* matches cam side */
#define PREVIEW_HTTP_TIMEOUT_MS  1500
#define PREVIEW_PERIOD_MS        200            /* 5 Hz */

static struct {
    bool                 running;
    EventGroupHandle_t   evt;        /* bit 0 = stop request */
    TaskHandle_t         task;
    lv_obj_t            *canvas;
    uint16_t             canvas_w;
    uint16_t             canvas_h;
    jpeg_decoder_handle_t decoder;
    uint8_t             *jpeg_buf;   /* PSRAM, holds the response body */
    uint32_t             jpeg_len;
    uint8_t             *rgb_buf;    /* PSRAM, RGB565 output */
    uint32_t             rgb_size;
    uint64_t             last_ok_ms;
} s = {};

#define EVT_STOP  BIT0

/* ── HTTP receive: append response chunks into the JPEG buffer ─────── */
static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s.jpeg_buf && s.jpeg_len + evt->data_len <= PREVIEW_FETCH_BUF_SIZE) {
            memcpy(s.jpeg_buf + s.jpeg_len, evt->data, evt->data_len);
            s.jpeg_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
    case HTTP_EVENT_DISCONNECTED:
        break;
    default:
        break;
    }
    return ESP_OK;
}

static bool fetch_one(const char *cam_ip, uint16_t cam_port)
{
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%u/preview.jpg",
             cam_ip, (unsigned)(cam_port ? cam_port : 80));

    esp_http_client_config_t cfg = {};
    cfg.url            = url;
    cfg.timeout_ms     = PREVIEW_HTTP_TIMEOUT_MS;
    cfg.event_handler  = http_evt;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size    = 4096;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    s.jpeg_len = 0;
    esp_err_t err = esp_http_client_perform(cli);
    int code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK) {
        ESP_LOGD(TAG, "fetch err: %s", esp_err_to_name(err));
        return false;
    }
    if (code != 200) {
        ESP_LOGD(TAG, "fetch http %d", code);
        return false;
    }
    if (s.jpeg_len < 64) return false;
    return true;
}

/* ── Decode + paint helper. Runs on the worker task; takes the LVGL
 *    BSP lock just for the canvas pointer swap so the LVGL renderer
 *    stays consistent. ─────────────────────────────────────────────── */
static bool decode_and_paint(void)
{
    if (!s.decoder) return false;

    jpeg_decode_cfg_t dec_cfg = {};
    dec_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    dec_cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s.decoder, &dec_cfg,
        s.jpeg_buf, s.jpeg_len,
        s.rgb_buf, s.rgb_size, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_decoder_process: %s", esp_err_to_name(err));
        return false;
    }
    if (out_size == 0) return false;

    bsp_display_lock(0);
    if (s.canvas && lv_obj_is_valid(s.canvas)) {
        /* Buffer is at native JPEG resolution (cam captures 1080p). The
         * canvas is sized smaller (e.g. 640×360); LVGL will scale via
         * its image rescale path. We swap the canvas data pointer
         * straight at our PSRAM buffer — no per-frame copy. */
        lv_canvas_set_buffer(s.canvas, s.rgb_buf, s.canvas_w, s.canvas_h,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s.canvas);
    }
    bsp_display_unlock();
    return true;
}

/* ── Worker task ───────────────────────────────────────────────────── */
static void worker(void *arg)
{
    (void)arg;
    while (true) {
        if ((xEventGroupGetBits(s.evt) & EVT_STOP) != 0) break;

        CamLinkInfo info = cam_link_get_info();
        if (info.link_up && info.status.wifi_sta_up &&
            info.status.ip_addr[0] != '\0' &&
            strcmp(info.status.ip_addr, "0.0.0.0") != 0) {
            if (fetch_one(info.status.ip_addr, info.status.http_port) &&
                decode_and_paint()) {
                s.last_ok_ms = (uint64_t)(esp_timer_get_time() / 1000);
            }
        }

        /* Wait for either the stop signal or the period tick. */
        if (xEventGroupWaitBits(s.evt, EVT_STOP, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(PREVIEW_PERIOD_MS)) & EVT_STOP) break;
    }
    s.task = nullptr;
    vTaskDelete(nullptr);
}

/* ── Public API ────────────────────────────────────────────────────── */
void cam_preview_open(lv_obj_t *canvas, uint16_t w, uint16_t h)
{
    if (s.running) cam_preview_close();
    if (!canvas || w == 0 || h == 0) return;

    s.canvas   = canvas;
    s.canvas_w = w;
    s.canvas_h = h;
    s.last_ok_ms = 0;

    /* RGB565 output buffer sized for the canvas. PSRAM-only; the JPEG
     * engine's DMA descriptors handle the cache flush for us. */
    s.rgb_size = (uint32_t)w * h * 2;
    s.rgb_buf = (uint8_t *)heap_caps_aligned_alloc(64, s.rgb_size,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s.jpeg_buf = (uint8_t *)heap_caps_malloc(PREVIEW_FETCH_BUF_SIZE,
                              MALLOC_CAP_SPIRAM);
    if (!s.rgb_buf || !s.jpeg_buf) {
        ESP_LOGE(TAG, "buffer alloc failed (rgb=%lu jpeg=%lu)",
                 (unsigned long)s.rgb_size, (unsigned long)PREVIEW_FETCH_BUF_SIZE);
        cam_preview_close();
        return;
    }

    /* HW JPEG decoder. Timeout matches the laptimer's old video
     * pipeline tuning — 200 ms is generous at 1080p with cache flush. */
    jpeg_decode_engine_cfg_t dec_cfg = {};
    dec_cfg.intr_priority = 0;
    dec_cfg.timeout_ms    = 200;
    if (jpeg_new_decoder_engine(&dec_cfg, &s.decoder) != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decoder engine create failed");
        cam_preview_close();
        return;
    }

    s.evt = xEventGroupCreate();
    if (!s.evt) { cam_preview_close(); return; }

    s.running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(worker, "cam_prev",
        4096, nullptr, 3, &s.task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        cam_preview_close();
        return;
    }
    ESP_LOGI(TAG, "preview started (%ux%u)", w, h);
}

void cam_preview_close(void)
{
    if (s.task && s.evt) {
        xEventGroupSetBits(s.evt, EVT_STOP);
        /* Worker self-deletes; wait up to 1 s for that to happen so
         * we don't free the buffers it might still be touching. */
        for (int i = 0; i < 20 && s.task != nullptr; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (s.evt)     { vEventGroupDelete(s.evt); s.evt = nullptr; }
    if (s.decoder) { jpeg_del_decoder_engine(s.decoder); s.decoder = nullptr; }
    if (s.jpeg_buf){ heap_caps_free(s.jpeg_buf); s.jpeg_buf = nullptr; }
    if (s.rgb_buf) { heap_caps_free(s.rgb_buf);  s.rgb_buf  = nullptr; }
    s.canvas = nullptr;
    s.running = false;
}

bool cam_preview_has_signal(void)
{
    if (!s.running) return false;
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    return (now - s.last_ok_ms) < 2000;
}
