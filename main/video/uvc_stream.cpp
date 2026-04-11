/**
 * uvc_stream.cpp -- USB Video Class camera driver
 *
 * Uses ESP-IDF USB Host stack with the UVC class driver to enumerate
 * and stream MJPEG frames from USB cameras.
 */

#include "uvc_stream.h"
#include "../compat.h"

#include "usb/usb_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "uvc_stream";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_usb_host_installed = false;
static bool s_camera_connected = false;
static bool s_streaming = false;
static uvc_frame_cb_t s_frame_cb = nullptr;

static UvcResolution s_resolutions[UVC_MAX_RESOLUTIONS];
static int s_n_resolutions = 0;
static int s_selected_res = 0;
static uint16_t s_active_w = 0, s_active_h = 0;
static uint8_t  s_active_fps = 0;

// MJPEG frame reassembly buffer (PSRAM)
#define MAX_FRAME_SIZE  (512 * 1024)   // 512 KB max for one MJPEG frame
static uint8_t *s_frame_buf = nullptr;
static uint32_t s_frame_len = 0;

// USB Host client handle
static usb_host_client_handle_t s_client_hdl = nullptr;
static usb_device_handle_t      s_dev_hdl = nullptr;
static SemaphoreHandle_t        s_usb_mutex = nullptr;

// ---------------------------------------------------------------------------
// USB Host library task — processes USB events
// ---------------------------------------------------------------------------
static void usb_host_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            // All clients deregistered
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            // All devices freed
        }
    }
}

// ---------------------------------------------------------------------------
// USB Host client event callback
// ---------------------------------------------------------------------------
static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            log_i("USB device connected (addr=%d)", event_msg->new_dev.address);
            // Try to open and enumerate
            if (!s_camera_connected) {
                esp_err_t err = usb_host_device_open(s_client_hdl,
                    event_msg->new_dev.address, &s_dev_hdl);
                if (err == ESP_OK) {
                    s_camera_connected = true;
                    log_i("USB camera opened");

                    // Get device descriptor for logging
                    const usb_device_desc_t *desc;
                    usb_host_get_device_descriptor(s_dev_hdl, &desc);
                    log_i("USB device: VID=0x%04X PID=0x%04X class=%d",
                          desc->idVendor, desc->idProduct, desc->bDeviceClass);

                    // Enumerate UVC resolutions from config descriptor
                    const usb_config_desc_t *cfg_desc;
                    usb_host_get_active_config_descriptor(s_dev_hdl, &cfg_desc);

                    // Parse UVC descriptors for MJPEG format and frame sizes
                    s_n_resolutions = 0;
                    const uint8_t *p = (const uint8_t *)cfg_desc;
                    const uint8_t *end = p + cfg_desc->wTotalLength;
                    uint8_t mjpeg_format_idx = 0;

                    while (p < end && s_n_resolutions < UVC_MAX_RESOLUTIONS) {
                        uint8_t bLength = p[0];
                        if (bLength == 0) break;

                        // UVC VS_FORMAT_MJPEG descriptor (CS_INTERFACE, subtype 0x06)
                        if (bLength >= 11 && p[1] == 0x24 && p[2] == 0x06) {
                            mjpeg_format_idx = p[3];
                            log_i("Found MJPEG format (index=%d)", mjpeg_format_idx);
                        }

                        // UVC VS_FRAME_MJPEG descriptor (CS_INTERFACE, subtype 0x07)
                        if (bLength >= 26 && p[1] == 0x24 && p[2] == 0x07 && mjpeg_format_idx > 0) {
                            UvcResolution &r = s_resolutions[s_n_resolutions];
                            r.format_idx = mjpeg_format_idx;
                            r.frame_idx = p[3];
                            r.width = p[5] | (p[6] << 8);
                            r.height = p[7] | (p[8] << 8);
                            // Default frame interval (100ns units) at offset 21
                            uint32_t interval = p[21] | (p[22] << 8) | (p[23] << 16) | (p[24] << 24);
                            r.fps = (interval > 0) ? (uint8_t)(10000000 / interval) : 30;
                            log_i("  Resolution: %dx%d @ %d fps (frame_idx=%d)",
                                  r.width, r.height, r.fps, r.frame_idx);
                            s_n_resolutions++;
                        }

                        p += bLength;
                    }

                    if (s_n_resolutions == 0) {
                        log_w("No MJPEG resolutions found — camera may use YUV only");
                    } else {
                        // Default: select highest resolution
                        s_selected_res = s_n_resolutions - 1;
                    }
                }
            }
            break;

        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            log_w("USB device disconnected");
            s_camera_connected = false;
            s_streaming = false;
            if (s_dev_hdl) {
                usb_host_device_close(s_client_hdl, s_dev_hdl);
                s_dev_hdl = nullptr;
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void uvc_stream_init(void)
{
    if (s_usb_host_installed) return;

    // Allocate frame buffer in PSRAM
    s_frame_buf = (uint8_t *)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_frame_buf) {
        log_e("Failed to allocate MJPEG frame buffer");
        return;
    }

    s_usb_mutex = xSemaphoreCreateMutex();

    // Install USB Host Library
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        log_e("usb_host_install failed: %s", esp_err_to_name(err));
        return;
    }

    // Start USB Host lib task
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host", 4096, nullptr, 10, nullptr, 0);

    // Register as USB Host client
    usb_host_client_config_t client_config = {};
    client_config.is_synchronous = false;
    client_config.max_num_event_msg = 5;
    client_config.async.client_event_callback = client_event_cb;
    client_config.async.callback_arg = nullptr;

    err = usb_host_client_register(&client_config, &s_client_hdl);
    if (err != ESP_OK) {
        log_e("usb_host_client_register failed: %s", esp_err_to_name(err));
        return;
    }

    // Start client event processing task
    xTaskCreatePinnedToCore([](void *arg) {
        (void)arg;
        while (true) {
            usb_host_client_handle_events(s_client_hdl, portMAX_DELAY);
        }
    }, "usb_client", 4096, nullptr, 5, nullptr, 0);

    s_usb_host_installed = true;
    log_i("USB Host + UVC initialized");
}

bool uvc_stream_connected(void)
{
    return s_camera_connected;
}

int uvc_stream_get_resolutions(UvcResolution *out, int max_count)
{
    int n = s_n_resolutions < max_count ? s_n_resolutions : max_count;
    if (n > 0 && out) {
        memcpy(out, s_resolutions, n * sizeof(UvcResolution));
    }
    return n;
}

void uvc_stream_set_resolution(int index)
{
    if (index >= 0 && index < s_n_resolutions) {
        s_selected_res = index;
        log_i("Resolution selected: %dx%d @ %d fps",
              s_resolutions[index].width, s_resolutions[index].height,
              s_resolutions[index].fps);
    }
}

bool uvc_stream_start(uvc_frame_cb_t cb)
{
    if (!s_camera_connected || s_n_resolutions == 0) {
        log_e("Cannot start stream: no camera or no resolutions");
        return false;
    }

    s_frame_cb = cb;
    const UvcResolution &r = s_resolutions[s_selected_res];
    s_active_w = r.width;
    s_active_h = r.height;
    s_active_fps = r.fps;

    // Claim the video streaming interface
    // For basic UVC cameras, interface 1 is typically the video streaming interface
    esp_err_t err = usb_host_interface_claim(s_client_hdl, s_dev_hdl, 1, 0);
    if (err != ESP_OK) {
        log_e("Failed to claim video interface: %s", esp_err_to_name(err));
        return false;
    }

    // Set alternate setting for the selected bandwidth
    // Most UVC cameras use alt setting > 0 for isochronous streaming
    // Try alt setting 1 first (maximum bandwidth)
    err = usb_host_interface_claim(s_client_hdl, s_dev_hdl, 1, 1);
    if (err != ESP_OK) {
        log_w("Alt setting 1 not available, using bulk transfer mode");
    }

    s_streaming = true;
    log_i("UVC streaming started: %dx%d @ %d fps", s_active_w, s_active_h, s_active_fps);

    // Start a task to read from the USB bulk/iso endpoint
    xTaskCreatePinnedToCore([](void *arg) {
        (void)arg;
        // Bulk transfer buffer
        const int xfer_size = 16384;
        uint8_t *xfer_buf = (uint8_t *)heap_caps_malloc(xfer_size, MALLOC_CAP_DMA);
        if (!xfer_buf) {
            log_e("Transfer buffer alloc failed");
            s_streaming = false;
            vTaskDelete(nullptr);
            return;
        }

        s_frame_len = 0;

        while (s_streaming && s_camera_connected) {
            // For bulk-mode UVC cameras, read from endpoint
            // Frame boundaries are detected by UVC payload headers
            usb_transfer_t *transfer;
            esp_err_t err = usb_host_transfer_alloc(xfer_size, 0, &transfer);
            if (err != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            transfer->device_handle = s_dev_hdl;
            transfer->bEndpointAddress = 0x81;  // EP1 IN (typical for UVC)
            transfer->callback = [](usb_transfer_t *t) {
                // Process UVC payload
                if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 2) {
                    const uint8_t *data = t->data_buffer;
                    int len = t->actual_num_bytes;

                    // UVC payload header
                    uint8_t hdr_len = data[0];
                    uint8_t hdr_flags = data[1];
                    bool end_of_frame = (hdr_flags & 0x02) != 0;

                    // Append payload data (after header) to frame buffer
                    if (hdr_len < len && s_frame_len + (len - hdr_len) < MAX_FRAME_SIZE) {
                        memcpy(s_frame_buf + s_frame_len, data + hdr_len, len - hdr_len);
                        s_frame_len += (len - hdr_len);
                    }

                    if (end_of_frame && s_frame_len > 0) {
                        // Complete frame received — invoke callback
                        if (s_frame_cb) {
                            s_frame_cb(s_frame_buf, s_frame_len);
                        }
                        s_frame_len = 0;
                    }
                }
            };
            transfer->num_bytes = xfer_size;

            err = usb_host_transfer_submit(transfer);
            if (err != ESP_OK) {
                usb_host_transfer_free(transfer);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // Wait for transfer completion
            vTaskDelay(pdMS_TO_TICKS(1));
            usb_host_transfer_free(transfer);
        }

        heap_caps_free(xfer_buf);
        vTaskDelete(nullptr);
    }, "uvc_read", 6144, nullptr, 5, nullptr, 0);

    return true;
}

void uvc_stream_stop(void)
{
    s_streaming = false;
    s_frame_cb = nullptr;

    if (s_dev_hdl && s_client_hdl) {
        usb_host_interface_release(s_client_hdl, s_dev_hdl, 1);
    }

    log_i("UVC streaming stopped");
}

void uvc_stream_get_active_resolution(uint16_t *w, uint16_t *h, uint8_t *fps)
{
    if (w) *w = s_active_w;
    if (h) *h = s_active_h;
    if (fps) *fps = s_active_fps;
}
