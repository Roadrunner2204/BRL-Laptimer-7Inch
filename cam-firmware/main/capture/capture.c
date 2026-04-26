/**
 * capture.c — MIPI-CSI capture pipeline scaffold for the BRL cam.
 *
 * Structure is final; the V4L2 ioctl calls below are written against
 * the esp_video API as documented in espressif/esp-video-components
 * around v0.7.x (matches ESP-IDF v5.4 component manager). When the
 * actual board arrives, expect to:
 *
 *   1. Adjust the device-node name ("/dev/video0") if esp_video changes
 *      it for the ESP32-P4 target.
 *   2. Tune V4L2_PIX_FMT_* — current code requests RAW10 from the
 *      sensor and lets the hardware ISP convert to RGB565 internally,
 *      which the JPEG encoder accepts. If your esp_video build emits
 *      JPEG natively (encoder-on-CSI mode) the ioctl path collapses
 *      down to "VIDIOC_DQBUF → recorder_push_jpeg_frame" — the rest
 *      of this code drops out.
 *   3. Provide pinmux + reset-pin GPIOs in sdkconfig if the BSP for
 *      the DFR1172 doesn't ship those defaults.
 *
 * Until then, capture_init() returns false (sensor not detected) so
 * recorder_has_sensor() reports false and the laptimer's STATUS frame
 * shows cam_connected=0 — at which point the user knows to look here.
 */

#include "capture.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "../recorder/recorder.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "capture";

#define CAPTURE_DEV  "/dev/video0"
#define CAPTURE_BUF_COUNT  3            /* triple-buffer for steady 30 fps */

/* Latest-frame snapshot for the HTTP preview endpoint. Sized for a
 * full-res JPEG worst case (≈ 0.05 bpp at 1080p ≈ 130 KB; 256 KB gives
 * comfortable headroom for high-detail scenes). Allocated in PSRAM
 * because the HW JPEG output is too big for internal RAM. */
#define PREVIEW_BUF_SIZE  (256 * 1024)
static uint8_t          *s_preview_buf = NULL;
static uint32_t          s_preview_len = 0;
static SemaphoreHandle_t s_preview_mux = NULL;

static bool s_sensor_present = false;
static TaskHandle_t s_task = NULL;

/* Stash a frame into the preview buffer. Cheap fast-path when the
 * mutex is held by the HTTP handler — we just skip this frame, the
 * next one (33 ms later) will land. */
static void stash_preview(const uint8_t *src, uint32_t len)
{
    if (!s_preview_mux || !s_preview_buf) return;
    if (len > PREVIEW_BUF_SIZE) return;
    if (xSemaphoreTake(s_preview_mux, 0) != pdTRUE) return;
    memcpy(s_preview_buf, src, len);
    s_preview_len = len;
    xSemaphoreGive(s_preview_mux);
}

uint32_t capture_get_latest_jpeg(uint8_t *out, uint32_t max_size)
{
    if (!s_preview_mux || !s_preview_buf || !out) return 0;
    uint32_t copied = 0;
    if (xSemaphoreTake(s_preview_mux, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    if (s_preview_len > 0 && s_preview_len <= max_size) {
        memcpy(out, s_preview_buf, s_preview_len);
        copied = s_preview_len;
    }
    xSemaphoreGive(s_preview_mux);
    return copied;
}

static void preview_alloc_once(void)
{
    if (s_preview_buf) return;
    s_preview_buf = (uint8_t *)heap_caps_malloc(PREVIEW_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_preview_mux = xSemaphoreCreateMutex();
    if (!s_preview_buf || !s_preview_mux) {
        ESP_LOGE(TAG, "preview buf alloc failed (%d B PSRAM)", PREVIEW_BUF_SIZE);
    }
}

#ifdef CONFIG_ESP_VIDEO_ENABLE
/* ── Real implementation, gated on the esp_video component being
 *    pulled in via idf_component.yml. Kept behind ifdef so the cam-
 *    firmware still builds end-to-end without the hardware-side
 *    components installed (CI-style smoke build). */

#include "linux/videodev2.h"
#include "esp_video_init.h"        /* esp_video_init / config structs */
#include <sys/ioctl.h>
#include <sys/mman.h>

static int s_fd = -1;
static struct {
    void  *start;
    size_t length;
} s_bufs[CAPTURE_BUF_COUNT];
static int s_buf_count = 0;

static bool open_and_configure(void)
{
    s_fd = open(CAPTURE_DEV, O_RDONLY);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open(%s): errno=%d", CAPTURE_DEV, errno);
        return false;
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = CAPTURE_WIDTH;
    fmt.fmt.pix.height = CAPTURE_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;   /* preferred — encoder
                                                      directly emits JPEG */
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGW(TAG, "VIDIOC_S_FMT(JPEG) failed — falling back to ISP path");
        /* Fallback: capture RAW10 from sensor + run software encode.
         * This path is the bytequest-gemini PoC route — keep as a
         * safety net; performance will be worse than HW JPEG. */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10;
        if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "VIDIOC_S_FMT(SBGGR10) failed too — giving up");
            return false;
        }
    }

    /* Request and mmap CAPTURE_BUF_COUNT buffers. */
    struct v4l2_requestbuffers req = {0};
    req.count  = CAPTURE_BUF_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS: errno=%d", errno);
        return false;
    }
    s_buf_count = req.count;
    for (int i = 0; i < s_buf_count; i++) {
        struct v4l2_buffer b = {0};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) return false;
        s_bufs[i].length = b.length;
        s_bufs[i].start  = mmap(NULL, b.length, PROT_READ, MAP_SHARED,
                                s_fd, b.m.offset);
        if (s_bufs[i].start == MAP_FAILED) return false;
        if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) return false;
    }

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &t) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON: errno=%d", errno);
        return false;
    }
    return true;
}

static void capture_task(void *arg)
{
    (void)arg;
    for (;;) {
        struct v4l2_buffer b = {0};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_fd, VIDIOC_DQBUF, &b) != 0) {
            ESP_LOGW(TAG, "DQBUF errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (b.bytesused > 0 && b.index < (uint32_t)s_buf_count) {
            const uint8_t *frame = (const uint8_t *)s_bufs[b.index].start;
            /* Always snapshot for the preview endpoint — a phone or the
             * laptimer's Video Settings screen may be aiming the cam
             * even when no recording is active. */
            stash_preview(frame, b.bytesused);

            if (recorder_is_active()) {
                recorder_push_jpeg_frame(frame, b.bytesused);
            }
        }
        ioctl(s_fd, VIDIOC_QBUF, &b);
    }
}

bool capture_init(void)
{
    preview_alloc_once();
    /* esp_video_init wires up the CSI receiver + ISP + sensor I2C, then
     * registers the V4L2 device node. Config struct is target-specific
     * — adjust against the actual esp_video version on bring-up. */
    esp_video_init_config_t cfg = {};   /* TODO: fill csi/sensor pins */
    if (esp_video_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed");
        return false;
    }
    if (!open_and_configure()) return false;

    s_sensor_present = true;
    xTaskCreatePinnedToCore(capture_task, "cam_cap",
                            6144, NULL, 6, &s_task, 1);
    ESP_LOGI(TAG, "capture started: %dx%d @ %d fps",
             CAPTURE_WIDTH, CAPTURE_HEIGHT, CAPTURE_FPS);
    return true;
}

#else  /* !CONFIG_ESP_VIDEO_ENABLE */

bool capture_init(void)
{
    preview_alloc_once();
    ESP_LOGW(TAG, "esp_video not enabled — capture disabled. Add the "
                  "espressif/esp_video component and rebuild.");
    s_sensor_present = false;
    return false;
}

#endif  /* CONFIG_ESP_VIDEO_ENABLE */

bool capture_sensor_present(void) { return s_sensor_present; }
