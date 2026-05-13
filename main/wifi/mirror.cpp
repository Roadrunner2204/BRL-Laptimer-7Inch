/**
 * mirror.cpp -- see mirror.h
 *
 * Implementation notes:
 *
 * Capture path:
 *   bsp_display_lock
 *     lv_snapshot_take_to_draw_buf(active_screen, RGB565)  // re-renders into our buf
 *   bsp_display_unlock
 *   memcpy lvgl-buf -> DMA-capable JPEG input buf
 *   jpeg_encoder_process(RGB565 -> YUV420 JPEG, q=55)
 *
 * The snapshot returns the LOGICAL screen image (post-LVGL, pre-display-rotation).
 * The display itself is rotated 180° via lv_display_set_rotation, but that's a
 * concern of the panel flush path; the snapshot is always right-side-up. So the
 * phone shows the UI as designed, the user sees the same image on the laptimer
 * (just hardware-flipped by the panel), and tap coordinates from the phone map
 * 1:1 into LVGL's logical coordinate space without any rotation maths.
 *
 * Concurrency:
 *   - Single HW JPEG encoder => one streaming client at a time, gated by an
 *     atomic flag (s_stream_busy).
 *   - Touch queue is FreeRTOS, safe across HTTP task (Core 0) -> LVGL task (Core 1).
 *   - bsp_display_lock wraps lv_snapshot_take so we don't race the LVGL renderer.
 *
 * Buffer sizes:
 *   - RGB565 1024x600 = 1.20 MB input
 *   - JPEG output capped at 256 KB (q=55 typically yields 60-150 KB)
 *   - Both allocated with jpeg_alloc_encoder_mem so they meet HW DMA alignment.
 */

#include "mirror.h"

#include <atomic>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "driver/jpeg_encode.h"

static const char *TAG = "mirror";

#define MIRROR_W       BSP_LCD_H_RES   // 1024
#define MIRROR_H       BSP_LCD_V_RES   //  600
#define MIRROR_BPP     2               // RGB565
#define MIRROR_RGB_SZ  ((size_t)MIRROR_W * MIRROR_H * MIRROR_BPP)
#define MIRROR_JPG_SZ  ((size_t)(256 * 1024))
#define MIRROR_QUALITY 55

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct TouchEv { int16_t x; int16_t y; bool pressed; };

static jpeg_encoder_handle_t s_jpeg     = nullptr;
static uint8_t              *s_rgb_buf  = nullptr;   // DMA-capable RGB565 input
static size_t                s_rgb_sz   = 0;
static uint8_t              *s_jpg_buf  = nullptr;   // DMA-capable JPEG output
static size_t                s_jpg_sz   = 0;
static lv_draw_buf_t        *s_draw_buf = nullptr;   // LVGL-allocated snapshot target

static std::atomic<bool>     s_stream_busy { false };

static QueueHandle_t s_touch_q = nullptr;
static lv_indev_t   *s_indev   = nullptr;

// ---------------------------------------------------------------------------
// Virtual touch indev: drains one event per LVGL read tick, holds last state
// between events so press-drag-release sequences look continuous to LVGL.
//
// Rotation compensation: LVGL's indev_pointer_proc() unconditionally applies
// the inverse of the display rotation to incoming pointer coords. The GT911
// driver hands LVGL raw PANEL pixels and relies on this transform to get
// LOGICAL coords. Our app already sends LOGICAL coords -- if we pass them
// straight through, LVGL flips them again and the tap lands at the mirrored
// point. So we pre-invert by the same transform here, making LVGL's flip a
// no-op. Read the rotation each tick so a runtime orientation change still
// works (settings screen could expose this later).
// ---------------------------------------------------------------------------
static void mirror_indev_read(lv_indev_t * /*indev*/, lv_indev_data_t *data)
{
    static int16_t          last_x   = 0;
    static int16_t          last_y   = 0;
    static lv_indev_state_t last_st  = LV_INDEV_STATE_RELEASED;
    static lv_indev_state_t prev_log = LV_INDEV_STATE_RELEASED;
    static int              read_n   = 0;

    read_n++;
    bool drained = false;
    TouchEv ev;
    if (s_touch_q && xQueueReceive(s_touch_q, &ev, 0) == pdTRUE) {
        last_x  = ev.x;
        last_y  = ev.y;
        last_st = ev.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        drained = true;
    }

    int16_t out_x = last_x;
    int16_t out_y = last_y;
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_rotation_t rot = lv_display_get_rotation(disp);
        if (rot == LV_DISPLAY_ROTATION_180) {
            out_x = (int16_t)(MIRROR_W - 1 - out_x);
            out_y = (int16_t)(MIRROR_H - 1 - out_y);
        }
        // 90/270 not handled -- laptimer panel is mounted landscape and
        // only rotates 0/180°. If portrait support is ever needed, mirror
        // the axis-swap logic from lv_indev.c indev_pointer_proc().
    }
    data->point.x = out_x;
    data->point.y = out_y;
    data->state   = last_st;

    // Log on state transitions and on every queue drain. This way the user
    // can see in monitor output exactly where the chain breaks: drained=1
    // = we got an event from HTTP, state-change = LVGL is polling us and
    // the press/release reached the indev layer. If only the inject log
    // (in mirror_inject_touch) ever fires but never an indev_read log
    // here, LVGL is not polling our indev.
    if (drained || last_st != prev_log) {
        ESP_LOGI(TAG, "indev_read[%d] in=(%d,%d) out=(%d,%d) %s%s",
                 read_n, (int)last_x, (int)last_y, (int)out_x, (int)out_y,
                 last_st == LV_INDEV_STATE_PRESSED ? "PRESSED" : "RELEASED",
                 drained ? " <DRAIN>" : "");
        prev_log = last_st;
    }
}

void mirror_init(void)
{
    if (s_indev) return;  // idempotent

    s_touch_q = xQueueCreate(16, sizeof(TouchEv));
    if (!s_touch_q) {
        ESP_LOGE(TAG, "touch queue alloc failed");
        return;
    }

    bsp_display_lock(0);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, mirror_indev_read);
    lv_indev_set_display(s_indev, lv_display_get_default());
    lv_indev_set_mode(s_indev, LV_INDEV_MODE_TIMER);
    lv_indev_enable(s_indev, true);

    // Diagnostic: list all registered indevs so we can see whether our
    // virtual one is actually in the global indev list LVGL polls.
    int idx = 0;
    for (lv_indev_t *it = lv_indev_get_next(NULL); it; it = lv_indev_get_next(it)) {
        ESP_LOGI(TAG, "init: indev #%d type=%d %s",
                 idx, (int)lv_indev_get_type(it),
                 it == s_indev ? "<-- mirror" : "");
        idx++;
    }
    bsp_display_unlock();

    ESP_LOGI(TAG, "init: virtual touch indev ready (%d indevs total)", idx);
}

void mirror_inject_touch(int16_t x, int16_t y, bool pressed)
{
    if (!s_touch_q) {
        ESP_LOGW(TAG, "inject_touch: queue NULL (init not run?)");
        return;
    }
    if (x < 0)            x = 0;
    if (y < 0)            y = 0;
    if (x >= MIRROR_W)    x = MIRROR_W - 1;
    if (y >= MIRROR_H)    y = MIRROR_H - 1;

    TouchEv ev { x, y, pressed };
    if (xQueueSend(s_touch_q, &ev, 0) != pdTRUE) {
        // Queue full (rapid drag, indev reader stalled). Drop oldest so the
        // press/release pair stays balanced -- losing a release would freeze
        // LVGL in a "pressed" state until the next tap lands.
        TouchEv discard;
        xQueueReceive(s_touch_q, &discard, 0);
        xQueueSend(s_touch_q, &ev, 0);
    }
    ESP_LOGI(TAG, "inject (%d,%d) %s qd=%u",
             (int)x, (int)y, pressed ? "DOWN" : "UP",
             (unsigned)uxQueueMessagesWaiting(s_touch_q));
}

// ---------------------------------------------------------------------------
// Text injection: find the visible LVGL keyboard, type into its textarea.
//
// Every dialog in the laptimer that needs text input creates an lv_textarea
// + lv_keyboard pair and binds them via lv_keyboard_set_textarea. So the
// "currently focused textarea" question reduces to "which keyboard is on
// screen right now". Walking screen + layer_top in tree order finds the
// topmost (most recently created) keyboard, which is what the user just
// opened.
// ---------------------------------------------------------------------------
static lv_obj_t *find_keyboard_recursive(lv_obj_t *root)
{
    if (!root) return nullptr;
    if (lv_obj_check_type(root, &lv_keyboard_class) &&
        !lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        return root;
    }
    uint32_t cnt = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *r = find_keyboard_recursive(lv_obj_get_child(root, i));
        if (r) return r;
    }
    return nullptr;
}

static lv_obj_t *find_active_textarea(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) return nullptr;
    lv_obj_t *kb = find_keyboard_recursive(lv_screen_active());
    if (!kb) kb = find_keyboard_recursive(lv_display_get_layer_top(disp));
    if (!kb) return nullptr;
    return lv_keyboard_get_textarea(kb);
}

bool mirror_inject_text(const char *utf8)
{
    if (!utf8 || !*utf8) return false;
    // Strip ASCII control chars (0x00..0x1F + DEL 0x7F). They have no place
    // in a textarea -- a stray newline once hit /sessions because the phone
    // sent "name\n" on the keyboard's Submit, and the snprintf-built JSON
    // emitted the raw 0x0A inside the string value, breaking every JSON
    // parser that wasn't lenient (Hermes, Python requests). UTF-8 multibyte
    // sequences have all continuation bytes >= 0x80 so they survive.
    char clean[512];
    size_t out = 0;
    for (size_t i = 0; utf8[i] && out + 1 < sizeof(clean); i++) {
        unsigned char c = (unsigned char)utf8[i];
        if (c < 0x20 || c == 0x7F) continue;
        clean[out++] = (char)c;
    }
    clean[out] = '\0';
    if (out == 0) return false;

    bool hit = false;
    bsp_display_lock(0);
    lv_obj_t *ta = find_active_textarea();
    if (ta) {
        lv_textarea_add_text(ta, clean);
        hit = true;
    }
    bsp_display_unlock();
    return hit;
}

bool mirror_inject_backspace(void)
{
    bool hit = false;
    bsp_display_lock(0);
    lv_obj_t *ta = find_active_textarea();
    if (ta) {
        lv_textarea_delete_char(ta);
        hit = true;
    }
    bsp_display_unlock();
    return hit;
}

// ---------------------------------------------------------------------------
// Lazy-init JPEG encoder + DMA buffers + LVGL snapshot draw buffer.
// ---------------------------------------------------------------------------
static esp_err_t encoder_lazy_init(void)
{
    if (s_jpeg && s_rgb_buf && s_jpg_buf && s_draw_buf) return ESP_OK;

    if (!s_jpeg) {
        jpeg_encode_engine_cfg_t enc_cfg = {};
        enc_cfg.intr_priority = 0;
        enc_cfg.timeout_ms    = 200;
        esp_err_t err = jpeg_new_encoder_engine(&enc_cfg, &s_jpeg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jpeg_new_encoder_engine: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!s_rgb_buf) {
        jpeg_encode_memory_alloc_cfg_t in_cfg = {};
        in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
        s_rgb_buf = (uint8_t *)jpeg_alloc_encoder_mem(MIRROR_RGB_SZ, &in_cfg, &s_rgb_sz);
        if (!s_rgb_buf) {
            ESP_LOGE(TAG, "alloc RGB input buf (%u B) failed", (unsigned)MIRROR_RGB_SZ);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_jpg_buf) {
        jpeg_encode_memory_alloc_cfg_t out_cfg = {};
        out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
        s_jpg_buf = (uint8_t *)jpeg_alloc_encoder_mem(MIRROR_JPG_SZ, &out_cfg, &s_jpg_sz);
        if (!s_jpg_buf) {
            ESP_LOGE(TAG, "alloc JPEG output buf (%u B) failed", (unsigned)MIRROR_JPG_SZ);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_draw_buf) {
        bsp_display_lock(0);
        s_draw_buf = lv_draw_buf_create(MIRROR_W, MIRROR_H,
                                        LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        bsp_display_unlock();
        if (!s_draw_buf) {
            ESP_LOGE(TAG, "lv_draw_buf_create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "encoder_init OK (rgb=%u jpg=%u stride=%u)",
             (unsigned)s_rgb_sz, (unsigned)s_jpg_sz,
             (unsigned)s_draw_buf->header.stride);
    return ESP_OK;
}

bool mirror_acquire_stream(void)
{
    bool expected = false;
    if (!s_stream_busy.compare_exchange_strong(expected, true)) {
        return false;
    }
    if (encoder_lazy_init() != ESP_OK) {
        s_stream_busy = false;
        return false;
    }
    return true;
}

void mirror_release_stream(void)
{
    // Inject a synthetic release in case the phone disconnected mid-press
    // (WiFi drop, app backgrounded, hard kill). Without this, LVGL would
    // sit in the PRESSED state until the next mirror session sends a real
    // release -- in the meantime the on-device touchscreen would feel
    // half-stuck because LVGL ignores new presses while one is active.
    mirror_inject_touch(0, 0, false);
    s_stream_busy = false;
}

// ---------------------------------------------------------------------------
// Capture: snapshot + memcpy + HW JPEG encode.
// ---------------------------------------------------------------------------
esp_err_t mirror_capture_jpeg(uint8_t **out_buf, size_t *out_size)
{
    if (!s_jpeg || !s_rgb_buf || !s_jpg_buf || !s_draw_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    bsp_display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_result_t lr = lv_snapshot_take_to_draw_buf(
        scr, LV_COLOR_FORMAT_RGB565, s_draw_buf);
    if (lr != LV_RESULT_OK) {
        bsp_display_unlock();
        ESP_LOGW(TAG, "lv_snapshot_take_to_draw_buf failed");
        return ESP_FAIL;
    }
    // Copy LVGL buf -> DMA-capable JPEG input. LVGL stride may differ from
    // tightly-packed W*BPP because of alignment padding; copy row-by-row in
    // that case so the encoder sees a contiguous image.
    const uint32_t row_bytes = (uint32_t)MIRROR_W * MIRROR_BPP;
    const uint32_t stride    = s_draw_buf->header.stride;
    const uint8_t *src       = s_draw_buf->data;
    uint8_t       *dst       = s_rgb_buf;
    if (stride == row_bytes) {
        memcpy(dst, src, row_bytes * (uint32_t)MIRROR_H);
    } else {
        for (int y = 0; y < MIRROR_H; y++) {
            memcpy(dst + (uint32_t)y * row_bytes,
                   src + (uint32_t)y * stride,
                   row_bytes);
        }
    }
    bsp_display_unlock();

    jpeg_encode_cfg_t cfg = {};
    cfg.height        = MIRROR_H;
    cfg.width         = MIRROR_W;
    cfg.src_type      = JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample    = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = MIRROR_QUALITY;

    uint32_t out_n = 0;
    esp_err_t err = jpeg_encoder_process(
        s_jpeg, &cfg,
        s_rgb_buf, (uint32_t)MIRROR_RGB_SZ,
        s_jpg_buf, (uint32_t)s_jpg_sz, &out_n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_encoder_process: %s", esp_err_to_name(err));
        return err;
    }
    *out_buf  = s_jpg_buf;
    *out_size = out_n;
    return ESP_OK;
}
