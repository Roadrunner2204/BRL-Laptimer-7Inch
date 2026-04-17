/**
 * uvc_stream.cpp -- USB Video Class camera driver
 *
 * Uses ESP-IDF USB Host stack directly. Key fix: sends explicit
 * SET_INTERFACE control transfers to switch alt settings (camera
 * won't stream without this).
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

// State
static bool s_usb_host_installed = false;
static volatile bool s_camera_connected = false;
static volatile bool s_streaming = false;
static uvc_frame_cb_t s_frame_cb = nullptr;

static UvcResolution s_resolutions[UVC_MAX_RESOLUTIONS];
static int s_n_resolutions = 0;
static int s_selected_res = 0;
static uint16_t s_active_w = 0, s_active_h = 0;
static uint8_t  s_active_fps = 0;

#define MAX_FRAME_SIZE  (512 * 1024)
static uint8_t *s_frame_buf = nullptr;
static uint32_t s_frame_len = 0;
static uint32_t s_frames_delivered = 0;

static usb_host_client_handle_t s_client_hdl = nullptr;
static usb_device_handle_t      s_dev_hdl = nullptr;

static uint8_t s_ep_is_iso = 0;
static uint16_t s_ep_mps = 0;
static uint8_t s_ep_addr = 0;
static int s_best_alt = 0;

static SemaphoreHandle_t s_xfer_done = nullptr;   // used for control transfers
static SemaphoreHandle_t s_stream_ready = nullptr; // signals when stream_task has allocated transfers
static QueueHandle_t     s_iso_done_q = nullptr;  // usb_transfer_t* of completed ISO xfers

// N=2 inflight transfers. N=4 crashed because it starved I2S DMA.
// N=1 capped fps at 5-10 even at 720p because the stack couldn't
// double-buffer. N=2 is the stable sweet spot.
#define ISO_XFERS_INFLIGHT  2

// 8 packets × base_mps bytes per transfer = good scheduling buffer.
// Tried 4 on 2026-04-16 with mult=2 alt — measured fps HALVED because
// the camera doubled frame size at higher quality instead of keeping fps,
// and smaller xfers gave the stream_task less tolerance for preemption.
#define ISO_PKTS_PER_XFER   8

// Largest USB ISO "mult" we're willing to negotiate.
// **MEASURED 2026-04-16**: this camera has a fixed ~3.5 Mbit/s MJPEG
// encoder cap. At mult=1 it delivers ~8 fps × 55 KB. At mult=2 it
// delivers ~4 fps × 120 KB (higher quality, same bitrate). mult=3
// would probably be the same pattern. Since fps matters more than
// per-frame quality for a laptimer, mult=1 is the right choice on this
// hardware. Left as a #define so a higher-end camera swap can flip it.
#define MAX_ISO_MULT        1

// USB ISO transfers — allocated ONCE at first successful start and reused
// across all subsequent start/stop cycles. The first allocation uses the
// freshest DMA budget. Once I2S (mic) takes its share, fragmentation makes
// re-allocating these 7.5 KB blocks impossible — so we never free them.
static usb_transfer_t *s_xfers[ISO_XFERS_INFLIGHT] = {};
static bool s_xfers_allocated = false;

// ---------------------------------------------------------------------------
static void usb_host_lib_task(void *arg) {
    (void)arg;
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

// ---------------------------------------------------------------------------
// Callback fires from usb_host event context when any transfer completes.
// For control transfers we use a binary semaphore (one at a time).
// For ISO transfers we use a queue so multiple in-flight transfers are OK.
static void xfer_cb(usb_transfer_t *transfer) {
    if (transfer && transfer->num_isoc_packets > 0 && s_iso_done_q) {
        xQueueSend(s_iso_done_q, &transfer, 0);
    } else if (s_xfer_done) {
        xSemaphoreGive(s_xfer_done);
    }
}

// ---------------------------------------------------------------------------
// Send a USB SET_INTERFACE request to switch alt setting
// ---------------------------------------------------------------------------
static bool send_set_interface(uint16_t interface, uint16_t alt_setting) {
    usb_transfer_t *ctrl;
    esp_err_t err = usb_host_transfer_alloc(8, 0, &ctrl);  // setup only, no data
    if (err != ESP_OK) return false;

    ctrl->device_handle = s_dev_hdl;
    ctrl->bEndpointAddress = 0;
    ctrl->callback = xfer_cb;
    ctrl->timeout_ms = 2000;
    ctrl->num_bytes = 8;  // just setup packet

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl->data_buffer;
    setup->bmRequestType = 0x01;  // Host-to-device, Standard, Interface
    setup->bRequest = 0x0B;       // SET_INTERFACE
    setup->wValue = alt_setting;
    setup->wIndex = interface;
    setup->wLength = 0;

    err = usb_host_transfer_submit_control(s_client_hdl, ctrl);
    if (err != ESP_OK) {
        usb_host_transfer_free(ctrl);
        return false;
    }
    xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(3000));
    bool ok = (ctrl->status == USB_TRANSFER_STATUS_COMPLETED);
    usb_host_transfer_free(ctrl);
    return ok;
}

// ---------------------------------------------------------------------------
// UVC Probe/Commit
// ---------------------------------------------------------------------------
static bool uvc_negotiate(const UvcResolution &r) {
    uint8_t probe[26] = {};
    probe[0] = 0x01;
    probe[2] = r.format_idx;
    probe[3] = r.frame_idx;
    uint32_t interval = (r.fps > 0) ? (10000000 / r.fps) : 333333;
    probe[4] = interval & 0xFF;
    probe[5] = (interval >> 8) & 0xFF;
    probe[6] = (interval >> 16) & 0xFF;
    probe[7] = (interval >> 24) & 0xFF;

    usb_transfer_t *ctrl;
    esp_err_t err = usb_host_transfer_alloc(8 + 26, 0, &ctrl);
    if (err != ESP_OK) return false;

    ctrl->device_handle = s_dev_hdl;
    ctrl->bEndpointAddress = 0;
    ctrl->callback = xfer_cb;
    ctrl->timeout_ms = 2000;

    // SET_CUR VS_PROBE_CONTROL
    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl->data_buffer;
    setup->bmRequestType = 0x21;
    setup->bRequest = 0x01;  // SET_CUR
    setup->wValue = (0x01 << 8);  // VS_PROBE_CONTROL
    setup->wIndex = 1;
    setup->wLength = 26;
    memcpy(ctrl->data_buffer + 8, probe, 26);
    ctrl->num_bytes = 8 + 26;

    err = usb_host_transfer_submit_control(s_client_hdl, ctrl);
    if (err != ESP_OK) { usb_host_transfer_free(ctrl); return false; }
    xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(3000));
    if (ctrl->status != USB_TRANSFER_STATUS_COMPLETED) {
        log_e("Probe failed: status=%d", ctrl->status);
        usb_host_transfer_free(ctrl); return false;
    }

    // SET_CUR VS_COMMIT_CONTROL
    setup->wValue = (0x02 << 8);  // VS_COMMIT_CONTROL
    memcpy(ctrl->data_buffer + 8, probe, 26);
    ctrl->num_bytes = 8 + 26;

    err = usb_host_transfer_submit_control(s_client_hdl, ctrl);
    if (err != ESP_OK) { usb_host_transfer_free(ctrl); return false; }
    xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(3000));
    bool ok = (ctrl->status == USB_TRANSFER_STATUS_COMPLETED);
    if (!ok) log_e("Commit failed: status=%d", ctrl->status);
    usb_host_transfer_free(ctrl);
    return ok;
}

// ---------------------------------------------------------------------------
// Process UVC payload
// ---------------------------------------------------------------------------
static void process_uvc_payload(const uint8_t *data, int len) {
    if (len < 2 || !s_frame_buf) return;
    uint8_t hdr_len   = data[0];
    uint8_t hdr_flags = data[1];
    bool    eof       = (hdr_flags & 0x02) != 0;
    uint8_t fid       = hdr_flags & 0x01;  // Frame ID toggle

    // Detect frame boundary via FID toggle — if it changed mid-frame, a packet
    // was dropped and the prior buffer is garbage. Flush and start over.
    static uint8_t s_prev_fid = 0xFF;
    if (s_prev_fid != 0xFF && s_prev_fid != fid && s_frame_len > 0) {
        s_frame_len = 0;  // FID toggled — prev frame incomplete
    }
    s_prev_fid = fid;

    if (hdr_len >= len) return;
    int plen = len - hdr_len;
    const uint8_t *pdata = data + hdr_len;

    // Start-of-frame sync: first payload bytes must be FFD8 (JPEG SOI).
    // If not, we're joining mid-frame (e.g. first packets after stream start,
    // or after a transient gap) — skip until a real frame start arrives.
    if (s_frame_len == 0) {
        if (plen < 2 || pdata[0] != 0xFF || pdata[1] != 0xD8) return;
    }

    // Scan payload for an in-stream SOI — indicates a new frame without
    // seeing EOF on the previous (cameras that don't toggle FID reliably).
    if (s_frame_len > 0) {
        for (int i = 0; i + 1 < plen; i++) {
            if (pdata[i] == 0xFF && pdata[i+1] == 0xD8) {
                // New frame begins mid-payload — discard old, restart from here
                s_frame_len = 0;
                pdata += i;
                plen  -= i;
                break;
            }
        }
    }

    if (s_frame_len + plen < MAX_FRAME_SIZE) {
        memcpy(s_frame_buf + s_frame_len, pdata, plen);
        s_frame_len += plen;
    } else {
        s_frame_len = 0;  // overflow — drop
    }

    if (eof && s_frame_len > 0) {
        if (s_frame_cb) {
            s_frame_cb(s_frame_buf, s_frame_len);
            s_frames_delivered++;
        }
        s_frame_len = 0;
    }
}

// ---------------------------------------------------------------------------
// Stream task — multiple transfers in-flight so ISO stream never idles.
// ---------------------------------------------------------------------------
static void stream_task(void *arg) {
    (void)arg;
    int base_mps = s_ep_mps & 0x7FF;
    if (base_mps == 0) base_mps = 512;
    int mult = ((s_ep_mps >> 11) & 0x03) + 1;
    int pkt_size = base_mps * mult;
    int num_pkts = ISO_PKTS_PER_XFER;
    int xfer_size = pkt_size * num_pkts;

    log_i("ISO stream: mps=%d×mult%d=%d B/µF, pkts=%d, size=%d, N=%d",
          base_mps, mult, pkt_size, num_pkts, xfer_size, ISO_XFERS_INFLIGHT);

    // Create completion queue (before any transfers exist to avoid race)
    if (!s_iso_done_q) {
        s_iso_done_q = xQueueCreate(ISO_XFERS_INFLIGHT * 2, sizeof(usb_transfer_t*));
    }
    if (!s_xfer_done) s_xfer_done = xSemaphoreCreateBinary();
    if (!s_stream_ready) s_stream_ready = xSemaphoreCreateBinary();

    // Allocate transfers ONCE — first start gets the freshest DMA budget.
    // Reused for all subsequent sessions so DMA fragmentation can't kill us.
    if (!s_xfers_allocated) {
        for (int i = 0; i < ISO_XFERS_INFLIGHT; i++) {
            esp_err_t err = ESP_FAIL;
            for (int attempt = 0; attempt < 5; attempt++) {
                err = usb_host_transfer_alloc(xfer_size, num_pkts, &s_xfers[i]);
                if (err == ESP_OK) break;
                log_w("xfer %d alloc try %d: %s (free DMA=%u)", i, attempt,
                      esp_err_to_name(err),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (err != ESP_OK) {
                log_e("Transfer %d alloc failed: %s (need %d bytes)", i,
                      esp_err_to_name(err), xfer_size);
                for (int k = 0; k < i; k++) {
                    if (s_xfers[k]) { usb_host_transfer_free(s_xfers[k]); s_xfers[k] = nullptr; }
                }
                s_streaming = false;
                if (s_stream_ready) xSemaphoreGive(s_stream_ready);
                vTaskDelete(nullptr); return;
            }
        }
        s_xfers_allocated = true;
        log_i("USB transfers allocated (persistent): %d × %d bytes", ISO_XFERS_INFLIGHT, xfer_size);
    }

    // Re-configure each session — device handle / EP may have re-enumerated
    for (int i = 0; i < ISO_XFERS_INFLIGHT; i++) {
        s_xfers[i]->device_handle = s_dev_hdl;
        s_xfers[i]->bEndpointAddress = s_ep_addr;
        s_xfers[i]->callback = xfer_cb;
        s_xfers[i]->num_bytes = xfer_size;
        s_xfers[i]->timeout_ms = 0;
        for (int p = 0; p < num_pkts; p++)
            s_xfers[i]->isoc_packet_desc[p].num_bytes = pkt_size;
    }

    // Drain any stale completions from a previous session
    usb_transfer_t *stale;
    while (xQueueReceive(s_iso_done_q, &stale, 0) == pdTRUE) {}

    // Signal that transfers are ready — brl_uvc_start() waits for this
    if (s_stream_ready) xSemaphoreGive(s_stream_ready);

    s_frame_len = 0;
    s_frames_delivered = 0;
    int warmup_xfers = 8;

    // Prime all transfers — they all run in parallel; ISO stream is continuous
    int inflight = 0;
    for (int i = 0; i < ISO_XFERS_INFLIGHT; i++) {
        if (usb_host_transfer_submit(s_xfers[i]) == ESP_OK) inflight++;
    }

    // Main loop: process completions, then resubmit.
    // ORDER MATTERS: we must READ the buffer BEFORE resubmitting — otherwise
    // the USB host starts overwriting our data mid-read. ISO continuity is
    // preserved by the OTHER transfer(s) still in flight during processing.
    while (s_streaming && s_camera_connected) {
        usb_transfer_t *done = nullptr;
        if (xQueueReceive(s_iso_done_q, &done, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (!done) continue;

        // Warmup: skip first N transfers (camera produces garbage at start)
        bool skip = (warmup_xfers > 0);
        if (skip) warmup_xfers--;

        if (!skip) {
            int offset = 0;
            for (int i = 0; i < num_pkts; i++) {
                int plen = done->isoc_packet_desc[i].actual_num_bytes;
                if (plen >= 2) process_uvc_payload(done->data_buffer + offset, plen);
                offset += pkt_size;
            }
        }

        // Now safe to resubmit — no more reads from this buffer
        if (s_streaming && s_camera_connected) {
            if (usb_host_transfer_submit(done) != ESP_OK) inflight--;
        } else {
            inflight--;
        }
    }

    // Drain remaining in-flight transfers so the next session can re-submit them.
    // Do NOT free — s_xfers[] is persistent across sessions.
    while (inflight > 0) {
        usb_transfer_t *done = nullptr;
        if (xQueueReceive(s_iso_done_q, &done, pdMS_TO_TICKS(500)) == pdTRUE) {
            inflight--;
        } else break;
    }

    log_i("Stream exit: %lu frames delivered", (unsigned long)s_frames_delivered);
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// USB client callback
// ---------------------------------------------------------------------------
static void client_event_cb(const usb_host_client_event_msg_t *ev, void *arg) {
    (void)arg;
    if (ev->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (s_camera_connected) return;
        esp_err_t err = usb_host_device_open(s_client_hdl, ev->new_dev.address, &s_dev_hdl);
        if (err != ESP_OK) return;

        const usb_device_desc_t *desc;
        usb_host_get_device_descriptor(s_dev_hdl, &desc);
        log_i("USB: VID=0x%04X PID=0x%04X class=%d", desc->idVendor, desc->idProduct, desc->bDeviceClass);

        const usb_config_desc_t *cfg;
        usb_host_get_active_config_descriptor(s_dev_hdl, &cfg);

        // Parse config: find MJPEG resolutions and best ISO endpoint
        const uint8_t *p = (const uint8_t *)cfg;
        const uint8_t *end = p + cfg->wTotalLength;
        uint8_t mjpeg_fmt = 0;
        int cur_iface = -1, cur_alt = -1;
        uint16_t best_mps = 0;

        s_n_resolutions = 0;
        s_best_alt = 1;
        s_ep_addr = 0;
        s_ep_mps = 0;
        s_ep_is_iso = 0;

        while (p < end) {
            uint8_t bLen = p[0]; if (bLen == 0) break;

            // Interface descriptor
            if (bLen >= 9 && p[1] == 0x04) { cur_iface = p[2]; cur_alt = p[3]; }

            // MJPEG format
            if (bLen >= 11 && p[1] == 0x24 && p[2] == 0x06) {
                mjpeg_fmt = p[3];
                log_i("MJPEG format (index=%d)", mjpeg_fmt);
            }

            // MJPEG frame — whitelist only the resolutions the product uses.
            // Recording targets: 1920x1080, 1440x1080, 1280x960, 1280x720.
            // Preview:            320x240. Everything else the camera
            // advertises (1024x768, 800x600, 640x480) is dropped so the UI
            // picker stays clean and the user can't land on a broken res.
            if (bLen >= 26 && p[1] == 0x24 && p[2] == 0x07 && mjpeg_fmt > 0
                && s_n_resolutions < UVC_MAX_RESOLUTIONS) {
                uint16_t w = p[5] | (p[6] << 8);
                uint16_t h = p[7] | (p[8] << 8);
                bool keep = (w == 1920 && h == 1080) ||
                            (w == 1440 && h == 1080) ||
                            (w == 1280 && h ==  960) ||
                            (w == 1280 && h ==  720) ||
                            (w ==  320 && h ==  240);
                if (keep) {
                    UvcResolution &r = s_resolutions[s_n_resolutions];
                    r.format_idx = mjpeg_fmt;
                    r.frame_idx = p[3];
                    r.width  = w;
                    r.height = h;
                    uint32_t iv = p[21] | (p[22] << 8) | (p[23] << 16) | (p[24] << 24);
                    r.fps = iv > 0 ? (uint8_t)(10000000 / iv) : 30;
                    log_i("  %dx%d @ %d fps  [kept]", w, h, r.fps);
                    s_n_resolutions++;
                } else {
                    log_i("  %dx%d  [dropped — not a product target]", w, h);
                }
            }

            // Endpoint on interface 1 (video streaming)
            if (bLen >= 7 && p[1] == 0x05 && cur_iface == 1 && cur_alt > 0) {
                uint8_t ep = p[2], attr = p[3];
                uint16_t raw_mps = p[4] | (p[5] << 8);
                uint16_t base_mps = raw_mps & 0x7FF;
                int mult = ((raw_mps >> 11) & 0x03) + 1;
                // Diagnostic dump: every ISO video-streaming alt-setting the
                // camera advertises. Used to check whether a higher-bandwidth
                // mult=2/3 alt exists that we're skipping — those would lift
                // the camera's effective MJPEG bitrate ceiling (~3.5 Mbit/s
                // on mult=1 means uniform ~8 fps at every recording res).
                if ((ep & 0x80) && (attr & 0x03) == 0x01) {
                    uint32_t band = (uint32_t)base_mps * mult * 8000;
                    log_i("  ISO alt=%d ep=0x%02X mps=%u×mult%d → %u B/µF (%lu B/s)",
                          cur_alt, ep, base_mps, mult, base_mps * mult,
                          (unsigned long)band);
                }
                if ((ep & 0x80) && (attr & 0x03) == 0x01 && mult <= MAX_ISO_MULT) {
                    // Pick highest total per-µframe bandwidth (base_mps * mult).
                    // mult>1 raises current draw — capped at MAX_ISO_MULT.
                    uint32_t this_band = (uint32_t)base_mps * mult;
                    uint32_t best_band = (uint32_t)best_mps *
                                         (((s_ep_mps >> 11) & 0x03) + 1);
                    if (this_band > best_band) {
                        best_mps = base_mps;
                        s_best_alt = cur_alt;
                        s_ep_addr = ep;
                        s_ep_mps = raw_mps;
                        s_ep_is_iso = 1;
                    }
                }
                if ((ep & 0x80) && (attr & 0x03) == 0x02 && !s_ep_addr) {
                    s_ep_addr = ep; s_ep_is_iso = 0;
                }
            }
            p += bLen;
        }

        // Default selection = largest recording resolution (skip the 320x240
        // preview entry). Descriptor order isn't guaranteed — pick by pixel
        // count. Falls back to last entry if only the preview slot matched.
        if (s_n_resolutions > 0) {
            int best = 0;
            uint32_t best_px = 0;
            for (int i = 0; i < s_n_resolutions; i++) {
                if (s_resolutions[i].width == 320 && s_resolutions[i].height == 240) continue;
                uint32_t px = (uint32_t)s_resolutions[i].width *
                              (uint32_t)s_resolutions[i].height;
                if (px > best_px) { best_px = px; best = i; }
            }
            s_selected_res = best_px > 0 ? best : s_n_resolutions - 1;
        }
        s_camera_connected = true;
        log_i("Camera ready: %d res, EP=0x%02X %s, alt=%d, MPS=%d",
              s_n_resolutions, s_ep_addr, s_ep_is_iso ? "ISO" : "BULK",
              s_best_alt, s_ep_mps & 0x7FF);

    } else if (ev->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        log_w("USB disconnected");
        s_camera_connected = false; s_streaming = false;
        if (s_dev_hdl) { usb_host_device_close(s_client_hdl, s_dev_hdl); s_dev_hdl = nullptr; }
    }
}

// ===========================================================================
// Public API
// ===========================================================================

void brl_uvc_init(void) {
    if (s_usb_host_installed) return;
    s_frame_buf = (uint8_t *)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_frame_buf) { log_e("Frame buf alloc failed"); return; }

    // Pre-create the stream-ready semaphore so brl_uvc_start() can block on it
    // BEFORE stream_task gets a chance to run. Otherwise the take is skipped
    // (semaphore still NULL) and start() returns true while alloc is pending.
    if (!s_stream_ready) s_stream_ready = xSemaphoreCreateBinary();

    usb_host_config_t hcfg = {}; hcfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&hcfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { log_e("install: %s", esp_err_to_name(err)); return; }

    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host", 4096, nullptr, 3, nullptr, 0);

    usb_host_client_config_t ccfg = {};
    ccfg.is_synchronous = false; ccfg.max_num_event_msg = 5;
    ccfg.async.client_event_callback = client_event_cb;
    usb_host_client_register(&ccfg, &s_client_hdl);

    xTaskCreatePinnedToCore([](void *) {
        while (true) usb_host_client_handle_events(s_client_hdl, portMAX_DELAY);
    }, "usb_evt", 4096, nullptr, 3, nullptr, 0);

    s_usb_host_installed = true;
    log_i("USB Host + UVC initialized (free DMA=%u)",
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

bool brl_uvc_connected(void) { return s_camera_connected; }

int brl_uvc_get_resolutions(UvcResolution *out, int max) {
    int n = s_n_resolutions < max ? s_n_resolutions : max;
    if (n > 0 && out) memcpy(out, s_resolutions, n * sizeof(UvcResolution));
    return n;
}

void brl_uvc_set_resolution(int idx) {
    if (idx >= 0 && idx < s_n_resolutions) {
        s_selected_res = idx;
        log_i("Res: %dx%d@%d", s_resolutions[idx].width, s_resolutions[idx].height, s_resolutions[idx].fps);
    }
}

bool brl_uvc_start(uvc_frame_cb_t cb) {
    if (!s_camera_connected || !s_ep_addr || s_n_resolutions == 0) {
        log_e("Cannot start"); return false;
    }

    const UvcResolution &r = s_resolutions[s_selected_res];
    s_frame_cb = cb; s_active_w = r.width; s_active_h = r.height; s_active_fps = r.fps;

    if (!s_xfer_done) s_xfer_done = xSemaphoreCreateBinary();

    // Step 1: Claim interface at alt 0 (zero bandwidth) for negotiation
    esp_err_t claim_err = usb_host_interface_claim(s_client_hdl, s_dev_hdl, 1, 0);
    if (claim_err != ESP_OK) {
        // Interface might still be claimed — release and retry
        usb_host_interface_release(s_client_hdl, s_dev_hdl, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        claim_err = usb_host_interface_claim(s_client_hdl, s_dev_hdl, 1, 0);
        if (claim_err != ESP_OK) {
            log_e("Cannot claim interface 1: %s", esp_err_to_name(claim_err));
            return false;
        }
    }
    if (!send_set_interface(1, 0)) log_w("SET_INTERFACE(1,0) failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    // Step 2: UVC Probe/Commit negotiation
    if (!uvc_negotiate(r)) {
        log_w("UVC negotiate failed — retrying after delay");
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!uvc_negotiate(r)) log_w("UVC negotiate retry failed — trying anyway");
    }

    // Step 3: Switch to high-bandwidth alt (THIS makes camera stream!)
    usb_host_interface_release(s_client_hdl, s_dev_hdl, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    usb_host_interface_claim(s_client_hdl, s_dev_hdl, 1, s_best_alt);
    if (!send_set_interface(1, s_best_alt)) {
        log_e("SET_INTERFACE(1,%d) FAILED", s_best_alt);
        return false;
    }
    log_i("SET_INTERFACE(1,%d) OK — camera should be streaming now", s_best_alt);

    // Camera needs ~200ms after SET_INTERFACE before producing valid frames
    vTaskDelay(pdMS_TO_TICKS(300));

    // Semaphore must exist before stream_task starts — otherwise the take
    // below is skipped and start() returns success before alloc is checked.
    if (!s_stream_ready) s_stream_ready = xSemaphoreCreateBinary();
    // Drain any stale signal from a prior aborted run.
    xSemaphoreTake(s_stream_ready, 0);

    s_streaming = true;
    xTaskCreatePinnedToCore(stream_task, "uvc_read", 6144, nullptr, 4, nullptr, 0);

    // Wait for stream_task to either succeed or fail allocating transfers.
    // stream_task signals the semaphore in BOTH cases — on failure it sets
    // s_streaming=false first, so we check that to distinguish.
    if (xSemaphoreTake(s_stream_ready, pdMS_TO_TICKS(2000)) != pdTRUE) {
        log_e("Stream task did not signal within 2s");
        s_streaming = false;
        // Release the alt 6 interface so EP 0x81 doesn't leak — otherwise
        // the next start fails with "EP with 129 address already allocated".
        usb_host_interface_release(s_client_hdl, s_dev_hdl, 1);
        return false;
    }
    if (!s_streaming) {
        log_e("Stream task failed to allocate transfers");
        // Same EP-leak guard as above
        usb_host_interface_release(s_client_hdl, s_dev_hdl, 1);
        return false;
    }

    log_i("Streaming: %dx%d@%d EP=0x%02X alt=%d", s_active_w, s_active_h, s_active_fps, s_ep_addr, s_best_alt);
    return true;
}

void brl_uvc_stop(void) {
    s_streaming = false; s_frame_cb = nullptr;
    vTaskDelay(pdMS_TO_TICKS(200));  // wait for stream_task to exit

    if (s_dev_hdl && s_client_hdl) {
        // Step 1: Switch back to zero-bandwidth alt setting (stops camera)
        if (!send_set_interface(1, 0)) {
            log_w("SET_INTERFACE(1,0) failed on stop — device may need re-plug");
        }
        // Step 2: Release the streaming interface
        usb_host_interface_release(s_client_hdl, s_dev_hdl, 1);
        // Step 3: Re-claim at alt 0 so device is in a clean state for restart
        usb_host_interface_claim(s_client_hdl, s_dev_hdl, 1, 0);
        // Step 4: Give camera time to settle back to idle
        vTaskDelay(pdMS_TO_TICKS(200));
        usb_host_interface_release(s_client_hdl, s_dev_hdl, 1);
    }
    log_i("Stopped");
}

void brl_uvc_get_active_resolution(uint16_t *w, uint16_t *h, uint8_t *fps) {
    if (w) *w = s_active_w;
    if (h) *h = s_active_h;
    if (fps) *fps = s_active_fps;
}
