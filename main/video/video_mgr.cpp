/**
 * video_mgr.cpp -- Video recording manager
 *
 * Coordinates UVC camera, JPEG pipeline, data overlay, and AVI writer.
 * Manages state transitions: IDLE → PREVIEW → RECORDING → IDLE.
 *
 * Auto-start: when timing becomes active and camera is connected,
 * recording starts automatically (if configured).
 */

#include "video_mgr.h"
#include "uvc_stream.h"
#include "video_pipeline.h"
#include "avi_writer.h"
#include "overlay.h"
#include "../audio/mic.h"
#include "../data/lap_data.h"
#include "../storage/sd_mgr.h"
#include "../compat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "video_mgr";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static VideoState s_state = VIDEO_IDLE;
static bool s_pipeline_ready = false;
static uint32_t s_rec_start_ms = 0;
static bool s_auto_record = true;     // auto-start when a lap/run begins
// Edge tracker for auto-start/stop. Tracks g_state.timing.in_lap — NOT
// timing_active. Reason: on A-B tracks timing_active latches true on the
// first start-line crossing and stays true forever (so the UI keeps the
// last run's times visible). Using timing_active as the edge signal
// meant the 2nd and later A-B runs never produced a rising edge, so
// auto-start didn't fire. in_lap cleanly toggles per run on both A-B
// and circuits, which is what we actually want to hook into.
static bool s_was_in_lap = false;
static uint8_t s_quality = 80;
// Passthrough recording: write raw MJPEG frames straight to the AVI and let
// the Android app render the data overlay at playback time. Default ON —
// this frees the ESP to hit 30 fps at 1080p instead of ~5 fps through
// the decode/overlay/encode pipeline.
static bool s_passthrough = true;

// Resolution management
static UvcResolution s_resolutions[UVC_MAX_RESOLUTIONS];
static int s_n_resolutions = 0;
static int s_res_index = -1;

// Task handle for the monitor task
static TaskHandle_t s_monitor_task = nullptr;

// ── AVI writer serialization ──
// Three concurrent callers touch the avi_writer:
//   1) on_uvc_frame (USB task)   → avi_writer_write_frame
//   2) audio_writer_task         → avi_writer_write_audio
//   3) video_split_for_next_lap  → avi_writer_close + avi_writer_open
// avi_writer has no internal locking — we serialize everything here so the
// close+open swap is atomic vs. in-flight frame/audio writes.
static SemaphoreHandle_t s_avi_mux = nullptr;

// Currently open AVI: lap number (0 = manual/REC_) and filename stem
// ("<session_id>_lap<N>" or "REC_<ms>"), used by lap_timer to tag sessions.
static uint8_t  s_current_lap_no = 0;
static char     s_current_stem[64] = {};

// Idle threshold for closing the trailing (incomplete) lap video after
// timing has ended. We only count seconds where the car is below
// IDLE_SPEED_KMH as "idle"; any real driving resets the counter. This keeps
// the camera rolling through long laps (e.g. 8-12 min Nordschleife) where
// the driver may keep going after an accidental session-end, and still
// closes the file cleanly 5 min after the car actually stops.
#define INCOMPLETE_LAP_GRACE_MS  (5 * 60 * 1000)
#define IDLE_SPEED_KMH           10.0f

// ---------------------------------------------------------------------------
// UVC frame callback — called from USB context for each MJPEG frame
// ---------------------------------------------------------------------------
// ─── Video pipeline decoupling ─────────────────────────────────────────────
// USB-task must never block on decode/encode/SD — at 720p+ that takes longer
// than one ISO transfer, causing packet loss and garbage JPEGs.
// Architecture:
//   USB task  -> on_uvc_frame: memcpy MJPEG into a free slot, push slot index
//                to ready-queue, immediately return.
//   Pipeline task (Core 1): pop slot, run HW decode + overlay + encode + AVI
//                write at its own pace, return slot to free pool.
//   Slot count = 3: enough to absorb SD write spikes without blocking USB.

#define VID_SLOT_COUNT   3
#define VID_SLOT_SIZE    (512 * 1024)   // matches uvc_stream MAX_FRAME_SIZE

static uint8_t *s_slot_buf[VID_SLOT_COUNT] = {nullptr, nullptr, nullptr};
static uint32_t s_slot_size[VID_SLOT_COUNT] = {0, 0, 0};
static QueueHandle_t s_free_q  = nullptr;  // MJPEG free slots
static QueueHandle_t s_ready_q = nullptr;  // MJPEG ready-to-decode

// RGB pipeline (stage 2): decoded frames waiting for encode + SD write.
// Runs in parallel to the decoder so the HW encode engine is not idle
// while the decoder works on the next frame.
struct RgbReady { int slot; uint16_t w; uint16_t h; };
static QueueHandle_t s_rgb_free_q  = nullptr;  // RGB free slot indices
static QueueHandle_t s_rgb_ready_q = nullptr;  // RGB ready-to-encode (RgbReady)

static TaskHandle_t s_pipeline_task = nullptr;  // decode + overlay
static TaskHandle_t s_encode_task   = nullptr;  // encode + SD write
static volatile bool s_pipeline_run = false;
static uint32_t s_frames_dropped = 0;

// ── Passthrough async writer (2026-04-18) ──
// Without decoupling, fwrite() in avi_writer_write_frame blocked the USB
// stream_task for 3-30 ms per frame. With only N=2 ISO transfers in flight
// (≈1 ms of USB buffering) the controller ran out of scheduled transfers
// and lost microframes, capping fps far below what the camera emits.
// Architecture: on_uvc_frame copies the MJPEG into a 2 MB PSRAM ring and
// returns immediately. A dedicated writer task drains the ring onto the
// SD card at its own pace. At 1080p/22 KB per frame the ring holds ~90
// frames ≈3 s — large enough to absorb even pathological SD flush pauses.
#define VIDEO_RING_SIZE          (2 * 1024 * 1024)
static RingbufHandle_t  s_video_ring = nullptr;
static TaskHandle_t     s_video_writer_task = nullptr;
static volatile bool    s_video_writer_run = false;
static uint32_t         s_video_ring_dropped = 0;

// Per-frame scratch in DMA-capable internal RAM. The SDMMC driver needs
// a DMA-capable source to write from; handing it a PSRAM pointer (where
// the ring lives) made it call esp_dma_capable_malloc() per fwrite and
// that allocation ran out when internal DMA was already tight, failing
// every SD write. Copying each frame into this scratch first lets the
// driver DMA straight out of it. The same pattern is used by the audio
// writer (see audio_writer_task_fn above).
// NOTE: The old per-frame PSRAM scratch lived here. Moved into
// avi_writer.cpp where it's now a 48 KB internal-DMA chunk buffer
// used with a chunked-copy fwrite. Keeping the source of fwrite in
// internal DMA lets the SDMMC driver DMA straight out of it at full
// bus speed instead of the slow per-4KB transient-copy fallback it
// used for PSRAM sources. Writer task below passes the PSRAM ring
// pointer directly to avi_writer_write_frame().

static void video_writer_task_fn(void * /*arg*/)
{
    // Rolling stats for throughput diagnosis
    uint32_t stats_frames = 0;
    uint64_t stats_bytes = 0;
    uint64_t stats_fwrite_us = 0;

    while (s_video_writer_run) {
        // Yield one tick per iteration so the IDLE1 task can run and
        // reset the Core-1 task watchdog. Without this the writer
        // (pri 5) plus LVGL (pri 3-4) kept Core 1 permanently busy,
        // firing TWDT every 5 s. 1 ms per frame is ~3 % overhead at
        // 30 fps — negligible compared to the ~30 ms fwrite time.
        vTaskDelay(1);

        size_t item_size = 0;
        void *item = xRingbufferReceive(s_video_ring, &item_size,
                                        pdMS_TO_TICKS(100));
        if (!item) continue;
        // Hand the PSRAM ring pointer straight to avi_writer — it has
        // its own internal-DMA chunk buffer and stages the copy there
        // so SDMMC can DMA at full bus speed.
        if (s_avi_mux && xSemaphoreTake(s_avi_mux, portMAX_DELAY) == pdTRUE) {
            if (avi_writer_is_open()) {
                int64_t t0 = esp_timer_get_time();
                bool ok = avi_writer_write_frame((const uint8_t *)item, (uint32_t)item_size);
                uint64_t dt = (uint64_t)(esp_timer_get_time() - t0);
                if (ok) {  // only count successful writes for throughput math
                    stats_fwrite_us += dt;
                    stats_bytes += item_size;
                    stats_frames++;
                    if (stats_frames >= 30) {
                        uint64_t kbps = (stats_bytes * 1000) / (stats_fwrite_us ? stats_fwrite_us : 1);
                        uint32_t avg_us = (uint32_t)(stats_fwrite_us / stats_frames);
                        uint64_t memcpy_us = 0, fw_us = 0;
                        uint32_t chunks = 0;
                        avi_writer_diag_snapshot(&memcpy_us, &fw_us, &chunks);
                        uint32_t avg_memcpy = chunks ? (uint32_t)(memcpy_us / chunks) : 0;
                        uint32_t avg_fw     = chunks ? (uint32_t)(fw_us / chunks)     : 0;
                        log_i("Writer: 30 ok frames, avg %u KB, avg fwrite %lu us, ~%llu KB/s "
                              "| per-chunk: memcpy %lu us, f_write %lu us (%lu chunks)",
                              (unsigned)(stats_bytes / stats_frames / 1024),
                              (unsigned long)avg_us,
                              (unsigned long long)kbps,
                              (unsigned long)avg_memcpy,
                              (unsigned long)avg_fw,
                              (unsigned long)chunks);
                        stats_frames = 0;
                        stats_bytes = 0;
                        stats_fwrite_us = 0;
                    }
                }
            }
            xSemaphoreGive(s_avi_mux);
        }
        vRingbufferReturnItem(s_video_ring, item);
    }
    s_video_writer_task = nullptr;
    vTaskDelete(nullptr);
}

// Called from USB stream task — MUST be fast. No decode/SD here.
static void on_uvc_frame(const uint8_t *data, uint32_t size)
{
    if (s_state != VIDEO_RECORDING && s_state != VIDEO_PREVIEW) return;
    if (size == 0) return;

    // ── PASSTHROUGH PATH (recording) ─────────────────────────────────
    // Push the MJPEG into the ring and return immediately. The writer
    // task handles fwrite() + lap-split mutex behind the scenes. Never
    // block the USB stream_task here — doing so drops microframes.
    if (s_passthrough && s_state == VIDEO_RECORDING) {
        if (s_video_ring) {
            if (xRingbufferSend(s_video_ring, data, size, 0) != pdTRUE) {
                s_video_ring_dropped++;  // ring full — SD can't keep up
            }
        }
        return;
    }

    // ── PREVIEW PATH ─────────────────────────────────────────────────
    // HW JPEG decode into RGB565 preview buffer. LVGL reads that RGB565
    // via video_get_preview_frame() — direct blit, no decode in render path.
    if (s_state == VIDEO_PREVIEW) {
        if (s_pipeline_ready) {
            video_pipeline_process(data, size, /*recording=*/false);
        }
        return;
    }

    // ── NON-PASSTHROUGH RECORDING ────────────────────────────────────
    // Full pipeline: HW decode → overlay → HW encode → AVI
    if (!s_free_q || !s_ready_q || size > VID_SLOT_SIZE) return;

    int slot;
    if (xQueueReceive(s_free_q, &slot, 0) != pdTRUE) {
        s_frames_dropped++;
        return;
    }
    memcpy(s_slot_buf[slot], data, size);
    s_slot_size[slot] = size;
    if (xQueueSend(s_ready_q, &slot, 0) != pdTRUE) {
        xQueueSend(s_free_q, &slot, 0);
        s_frames_dropped++;
    }
}

// ── Stage 1: DECODE + OVERLAY task (Core 1, uses HW JPEG decoder engine) ──
static void pipeline_task_fn(void *arg)
{
    (void)arg;
    log_i("Decode task started on Core %d", (int)xPortGetCoreID());
    int mjpeg_slot;
    while (s_pipeline_run) {
        if (xQueueReceive(s_ready_q, &mjpeg_slot, pdMS_TO_TICKS(200)) != pdTRUE)
            continue;
        if (mjpeg_slot < 0 || mjpeg_slot >= VID_SLOT_COUNT) continue;

        bool recording = (s_state == VIDEO_RECORDING);
        bool need_overlay = recording;

        // Grab a free RGB slot (non-blocking — if none, drop the frame)
        int rgb_slot = -1;
        if (xQueueReceive(s_rgb_free_q, &rgb_slot, pdMS_TO_TICKS(50)) != pdTRUE) {
            // Encoder fell behind — drop this frame rather than block
            s_frames_dropped++;
            xQueueSend(s_free_q, &mjpeg_slot, 0);
            continue;
        }

        uint16_t fw = 0, fh = 0;
        bool ok = video_pipeline_decode_into(rgb_slot,
                                             s_slot_buf[mjpeg_slot],
                                             s_slot_size[mjpeg_slot],
                                             &fw, &fh, need_overlay);

        // Return MJPEG slot immediately — decoder is done with it
        xQueueSend(s_free_q, &mjpeg_slot, 0);

        if (!ok || fw == 0 || fh == 0) {
            xQueueSend(s_rgb_free_q, &rgb_slot, 0);
            continue;
        }

        if (recording) {
            // Queue the decoded RGB for the encoder stage
            RgbReady rr = { rgb_slot, fw, fh };
            if (xQueueSend(s_rgb_ready_q, &rr, 0) != pdTRUE) {
                // Encode ready queue full — return slot
                xQueueSend(s_rgb_free_q, &rgb_slot, 0);
                s_frames_dropped++;
            }
        } else {
            // Preview: RGB already visible via video_pipeline preview pointer,
            // free the slot again (not a persistent owner — next decode flips it)
            xQueueSend(s_rgb_free_q, &rgb_slot, 0);
        }
    }
    log_i("Decode task exit (drops=%lu)", (unsigned long)s_frames_dropped);
    s_pipeline_task = nullptr;
    vTaskDelete(nullptr);
}

// ── Stage 2: ENCODE + SD-WRITE task (Core 0, uses HW JPEG encoder engine) ──
// Runs in parallel to the decoder so the HW engines don't serialize on a
// single thread. Pinned to Core 0 where SDMMC/fatfs interrupts are handled.
static void encode_task_fn(void *arg)
{
    (void)arg;
    log_i("Encode task started on Core %d", (int)xPortGetCoreID());
    RgbReady rr;
    while (s_pipeline_run) {
        if (xQueueReceive(s_rgb_ready_q, &rr, pdMS_TO_TICKS(200)) != pdTRUE)
            continue;
        if (rr.slot < 0 || rr.slot >= VP_RGB_SLOT_COUNT) continue;

        if (s_state == VIDEO_RECORDING) {
            video_pipeline_encode_from(rr.slot, rr.w, rr.h);
        }
        xQueueSend(s_rgb_free_q, &rr.slot, 0);
    }
    log_i("Encode task exit");
    s_encode_task = nullptr;
    vTaskDelete(nullptr);
}

// Allocate slots + queues. Call once lazily before first use.
static bool pipeline_decoupler_init(void)
{
    if (s_free_q && s_rgb_free_q) return true;  // already done

    // MJPEG slots (input to decoder)
    for (int i = 0; i < VID_SLOT_COUNT; i++) {
        if (s_slot_buf[i]) continue;
        s_slot_buf[i] = (uint8_t *)heap_caps_malloc(VID_SLOT_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_slot_buf[i]) { log_e("MJPEG slot %d alloc failed", i); return false; }
    }
    if (!s_free_q)  s_free_q  = xQueueCreate(VID_SLOT_COUNT, sizeof(int));
    if (!s_ready_q) s_ready_q = xQueueCreate(VID_SLOT_COUNT, sizeof(int));
    if (!s_free_q || !s_ready_q) { log_e("MJPEG queue alloc failed"); return false; }

    // RGB slots (output of decoder, input to encoder) — video_pipeline owns buffers
    if (!s_rgb_free_q)  s_rgb_free_q  = xQueueCreate(VP_RGB_SLOT_COUNT, sizeof(int));
    if (!s_rgb_ready_q) s_rgb_ready_q = xQueueCreate(VP_RGB_SLOT_COUNT, sizeof(RgbReady));
    if (!s_rgb_free_q || !s_rgb_ready_q) { log_e("RGB queue alloc failed"); return false; }

    // Drain and re-prime free pools (idempotent)
    int tmp;
    RgbReady rr_tmp;
    while (xQueueReceive(s_free_q, &tmp, 0) == pdTRUE) {}
    while (xQueueReceive(s_ready_q, &tmp, 0) == pdTRUE) {}
    while (xQueueReceive(s_rgb_free_q, &tmp, 0) == pdTRUE) {}
    while (xQueueReceive(s_rgb_ready_q, &rr_tmp, 0) == pdTRUE) {}
    for (int i = 0; i < VID_SLOT_COUNT; i++)    xQueueSend(s_free_q, &i, 0);
    for (int i = 0; i < VP_RGB_SLOT_COUNT; i++) xQueueSend(s_rgb_free_q, &i, 0);
    return true;
}

// ─── Audio decoupling: ring buffer + writer task ───────────────────────────
// Problem: SD fwrite() for a 60KB video frame can block 20-50ms.
// mic_task and video task share the same FILE*, so mic_task blocks during
// video writes. While blocked, I2S DMA ring overflows → audible ticks.
// Fix: mic_task pushes PCM bytes into a stream buffer (memcpy, never blocks),
// a dedicated writer task drains the buffer into the AVI on its own schedule.
#include "freertos/stream_buffer.h"

#define AUDIO_SB_BYTES     (128 * 1024)  // ~1.4s at 22050 Hz stereo 16-bit
#define AUDIO_WRITE_BYTES  (8192)        // 2048 sample-frames stereo 16-bit

static StreamBufferHandle_t s_audio_sb = nullptr;
static TaskHandle_t s_audio_writer_task = nullptr;
static volatile bool s_audio_writer_run = false;

static void audio_writer_task_fn(void *arg)
{
    (void)arg;
    static int16_t buf[AUDIO_WRITE_BYTES / 2];
    while (s_audio_writer_run) {
        size_t got = xStreamBufferReceive(s_audio_sb, buf,
                                          AUDIO_WRITE_BYTES, pdMS_TO_TICKS(200));
        if (got == 0) continue;
        if (s_state != VIDEO_RECORDING) continue;
        uint32_t frames = (uint32_t)(got / (mic_channels() * sizeof(int16_t)));
        if (!s_avi_mux || xSemaphoreTake(s_avi_mux, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (avi_writer_is_open()) avi_writer_write_audio(buf, frames);
        xSemaphoreGive(s_avi_mux);
    }
    s_audio_writer_task = nullptr;
    vTaskDelete(nullptr);
}

// Called from mic_task on Core 0 — must be fast (no SD I/O here)
static void on_mic_chunk(const int16_t *pcm, uint32_t samples)
{
    if (!s_audio_sb) return;
    // mic delivers STEREO interleaved 16-bit PCM: samples × channels × 2 B.
    size_t bytes = samples * mic_channels() * sizeof(int16_t);
    xStreamBufferSend(s_audio_sb, pcm, bytes, 0);
}

// ---------------------------------------------------------------------------
// Build AVI path + stem for a given lap number.
// lap_no == 0 → manual recording, no session context: REC_<ms>.avi
// lap_no >  0 + session_id set → <session_id>_lap<N>.avi
// lap_no >  0 + no session_id  → falls back to REC_<ms>_lap<N>.avi so we
//                                 at least keep the lap number in the name.
// Fills both `path_out` (full VFS path for fopen) and `stem_out` (basename
// without .avi, used by the HTTP /video/<id> route).
// ---------------------------------------------------------------------------
static void build_rec_path(uint8_t lap_no,
                           char *path_out, size_t path_len,
                           char *stem_out, size_t stem_len)
{
    const char *sid = g_state.session.session_id;
    if (lap_no > 0 && sid[0]) {
        snprintf(stem_out, stem_len, "%s_lap%u", sid, (unsigned)lap_no);
    } else if (lap_no > 0) {
        snprintf(stem_out, stem_len, "REC_%lu_lap%u",
                 (unsigned long)millis(), (unsigned)lap_no);
    } else {
        snprintf(stem_out, stem_len, "REC_%lu", (unsigned long)millis());
    }
    snprintf(path_out, path_len, "/sdcard/videos/%s.avi", stem_out);
}

// ---------------------------------------------------------------------------
// Monitor task: watches for auto-record triggers
// ---------------------------------------------------------------------------
static void video_monitor_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));

        bool camera_ready = brl_uvc_connected() && sd_mgr_available();
        bool in_lap_now   = g_state.timing.in_lap;
        bool rising       = in_lap_now && !s_was_in_lap;
        bool falling      = !in_lap_now && s_was_in_lap;

        // Auto-arm DISABLED — added 2026-04-19 but regressed fps (camera
        // Q-factor bumps during long idle stream). Users can still call
        // video_arm() manually from UI if needed (e.g. before a hot lap).

        // ── Auto-start on rising edge of in_lap ──────────────────────
        // Accept both IDLE (fallback — warm-up delay in start_recording)
        // and ARMED (fast path — instant switch to RECORDING).
        if (rising && s_auto_record &&
            (s_state == VIDEO_IDLE || s_state == VIDEO_ARMED)) {
            if (!camera_ready) {
                continue;   // wait for camera, keep the edge armed
            }
            uint8_t lap_hint = g_state.timing.lap_number
                               ? g_state.timing.lap_number : 1;
            log_i("Auto-start recording (in_lap rising, lap=%u, from %s)",
                  lap_hint, s_state == VIDEO_ARMED ? "ARMED" : "IDLE");
            if (!video_start_recording(lap_hint)) {
                log_w("Auto-start failed — will retry next tick");
                continue;   // keep s_was_in_lap=false for retry
            }
        }

        // ── Auto-stop on falling edge of in_lap ──────────────────────
        // Activity-aware grace so the trailing lap finishes cleanly on
        // circuits. For A-B, lap_timer::finish_lap() calls
        // video_stop_recording() directly at the finish line, so by the
        // time the monitor sees the falling edge s_state is already
        // IDLE and this whole block is skipped.
        // For manual-started recordings on circuits this fires too,
        // because the tracker below is updated every tick.
        if (falling && s_state == VIDEO_RECORDING && s_auto_record) {
            log_i("Lap ended — close video after %d min below %.0f km/h",
                  INCOMPLETE_LAP_GRACE_MS / 60000, (double)IDLE_SPEED_KMH);
            uint32_t idle_ms = 0;
            while (idle_ms < INCOMPLETE_LAP_GRACE_MS) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (g_state.timing.in_lap)      break;   // new lap started
                if (s_state != VIDEO_RECORDING) break;   // manual stop
                float spd = g_state.gps.valid
                            ? g_state.gps.speed_kmh : 0.0f;
                if (spd >= IDLE_SPEED_KMH) {
                    idle_ms = 0;
                } else {
                    idle_ms += 1000;
                }
            }
            if (!g_state.timing.in_lap && s_state == VIDEO_RECORDING) {
                log_i("Auto-stop recording (idle %d min)",
                      INCOMPLETE_LAP_GRACE_MS / 60000);
                video_stop_recording();
            }
        }

        // Tracker updated every tick — read fresh after the (possibly
        // long) grace loop above, not the value captured at tick start.
        s_was_in_lap = g_state.timing.in_lap;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void video_init(void)
{
    // Create /sdcard/videos directory
    sd_make_dir("/videos");

    // ── DMA BUDGETING AT BOOT ──
    // The internal-DMA reserve (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL, now
    // 128 KB) is fought over by USB, I2S mic, SDMMC and the AVI chunk buf.
    // Fragmentation accumulates as the system runs — by record-start the
    // largest contiguous block is typically <6 KB, which kills every one
    // of these at their first allocation.
    // Strategy: pre-allocate all persistent DMA blocks here, ORDERED from
    // smallest to largest so each takes the minimum bite out of the
    // still-contiguous pool. Once they're all in, nothing of this class
    // is allocated at record time — start_recording just wires up handles.

    // 1) USB ISO transfer buffers (2 × 7552 B ≈ 15 KB) via brl_uvc_init().
    brl_uvc_init();

    // 2) I2S mic DMA descriptors (~16 KB) via bsp_audio_init under mic_init.
    //    Must happen at boot: attempting this after the pool has fragmented
    //    returns ESP_ERR_NO_MEM and the BSP leaks the partially-init'd I2S
    //    channel (i2s_del_channel: "channel can't be deleted unless it is
    //    disabled"), bleeding ~16 KB per failed attempt and permanently
    //    disabling audio for the session. Calling it while DMA is still
    //    fresh sidesteps both problems. Failure here is non-fatal — audio
    //    simply stays off for the session.
    (void)mic_init();

    // 3) AVI chunk buffer (16 KB, 128-byte aligned for SDMMC direct DMA).
    avi_writer_preallocate();

    // Mutex protecting avi_writer state across USB, audio, and split tasks.
    if (!s_avi_mux) s_avi_mux = xSemaphoreCreateMutex();

    // Passthrough async-writer: 2 MB PSRAM ring + draining task. Created
    // once and reused across all recordings so DMA fragmentation over
    // time cannot starve a later allocation.
    if (!s_video_ring) {
        s_video_ring = xRingbufferCreateWithCaps(VIDEO_RING_SIZE,
                                                  RINGBUF_TYPE_NOSPLIT,
                                                  MALLOC_CAP_SPIRAM);
        if (!s_video_ring) {
            log_e("Video ring alloc failed — passthrough will block stream_task");
        }
    }
    if (s_video_ring && !s_video_writer_task) {
        s_video_writer_run = true;
        // Core 1 at priority 3 (below LVGL pri 4 → UI stays smooth). pri 5
        // (original) made LVGL lag visibly; pri 2 starved the writer
        // (first fwrite 209 ms, ring dropped frames). pri 3 is the
        // compromise. Stack in internal DRAM — PSRAM stack caused
        // FreeRTOS/cache issues that manifested as DMA-pool exhaustion.
        xTaskCreatePinnedToCore(video_writer_task_fn, "vid_writer",
                                 4096, nullptr, 3, &s_video_writer_task, 1);
    }

    // Start monitor task. Priority MUST match logic_task (1) — not higher.
    // At priority 2 the A-B pre-roll's video_start_recording() blocks
    // logic_task mid-call (ES7210 init + USB SET_INTERFACE + AVI fopen
    // = 200-500 ms), during which gps_poll + lap_timer_poll can't run.
    // If the driver crosses S/F during that window the crossing is missed.
    xTaskCreatePinnedToCore(video_monitor_task, "vid_mon", 4096, nullptr, 1, &s_monitor_task, 0);

    log_i("Video manager initialized");
}

void video_arm(void)
{
    if (s_state == VIDEO_ARMED || s_state == VIDEO_RECORDING) {
        return;  // already warm
    }
    if (!brl_uvc_connected()) {
        log_w("video_arm: no camera connected");
        return;
    }
    uint32_t t0 = millis();
    log_i("video_arm: starting UVC + warmup");

    // Pick default resolution if caller didn't set one yet.
    if (s_n_resolutions == 0) {
        s_n_resolutions = brl_uvc_get_resolutions(s_resolutions, UVC_MAX_RESOLUTIONS);
    }
    if (s_res_index < 0 || s_res_index >= s_n_resolutions) {
        int best = 0;
        uint32_t best_px = 0;
        for (int i = 0; i < s_n_resolutions; i++) {
            if (s_resolutions[i].width == 320 && s_resolutions[i].height == 240) continue;
            uint32_t px = (uint32_t)s_resolutions[i].width *
                          (uint32_t)s_resolutions[i].height;
            if (px > best_px) { best_px = px; best = i; }
        }
        s_res_index = best_px > 0 ? best : 0;
    }

    brl_uvc_set_resolution(s_res_index);
    if (!brl_uvc_start(on_uvc_frame)) {
        log_e("video_arm: UVC start failed");
        return;
    }
    // Warmup: on_uvc_frame discards frames while s_state != VIDEO_RECORDING.
    // 1500 ms lets AE/AWB/MJPEG-bitrate converge so the first real
    // recorded frame is already small (~8 KB).
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_state = VIDEO_ARMED;
    log_i("video_arm: ready after %lu ms — next record start is ~350 ms",
          (unsigned long)(millis() - t0));
}

void video_disarm(void)
{
    if (s_state != VIDEO_ARMED) return;
    log_i("video_disarm: stopping UVC");
    brl_uvc_stop();
    s_state = VIDEO_IDLE;
}

bool video_start_recording(uint8_t lap_hint)
{
    uint32_t t_start = millis();
    #define TLOG(fmt, ...) log_i("[+%4lu ms] " fmt, (unsigned long)(millis() - t_start), ##__VA_ARGS__)
    TLOG("video_start_recording: enter");

    if (s_state == VIDEO_RECORDING) {
        log_w("Already recording");
        return true;
    }

    if (!brl_uvc_connected()) {
        log_e("No camera connected");
        return false;
    }

    if (!sd_mgr_available()) {
        log_e("SD card not available");
        return false;
    }

    // Get resolution
    s_n_resolutions = brl_uvc_get_resolutions(s_resolutions, UVC_MAX_RESOLUTIONS);
    if (s_n_resolutions == 0) {
        log_e("No camera resolutions available");
        return false;
    }

    if (s_res_index < 0 || s_res_index >= s_n_resolutions) {
        // Default: highest-pixel recording resolution, excluding the 320x240
        // preview slot. With the whitelist filter in uvc_stream this is
        // always 1920x1080 unless the camera itself doesn't offer it.
        int best = 0;
        uint32_t best_px = 0;
        for (int i = 0; i < s_n_resolutions; i++) {
            if (s_resolutions[i].width == 320 && s_resolutions[i].height == 240) continue;
            uint32_t px = (uint32_t)s_resolutions[i].width *
                          (uint32_t)s_resolutions[i].height;
            if (px > best_px) { best_px = px; best = i; }
        }
        s_res_index = best_px > 0 ? best : s_n_resolutions - 1;
    }

    const UvcResolution &res = s_resolutions[s_res_index];

    // Initialize pipeline only if NOT in passthrough mode. In passthrough
    // we write raw MJPEG straight to the AVI and never touch the HW codec
    // or the large RGB buffers — saves ~5.5 MB PSRAM and all the DMA setup.
    if (!s_passthrough) {
        if (!s_pipeline_ready) {
            if (!video_pipeline_init(res.width, res.height)) {
                log_e("Pipeline init failed");
                return false;
            }
            s_pipeline_ready = true;
        }
        video_pipeline_set_quality(s_quality);
    }

    s_frames_dropped = 0;
    // Decoupled-pipeline tasks (decode + encode) only run in NON-passthrough
    // mode. Passthrough writes MJPEG straight to SD from the USB task — no
    // queues, no extra tasks, no HW codec usage.
    if (!s_passthrough) {
        if (!pipeline_decoupler_init()) {
            log_e("Pipeline decoupler init failed");
            return false;
        }
        s_pipeline_run = true;
        if (!s_pipeline_task) {
            xTaskCreatePinnedToCore(pipeline_task_fn, "vid_dec",
                                    4096, nullptr, 3, &s_pipeline_task, 1);
        }
        if (!s_encode_task) {
            xTaskCreatePinnedToCore(encode_task_fn, "vid_enc",
                                    4096, nullptr, 3, &s_encode_task, 0);
        }
    }

    // Camera bring-up: SKIP if already ARMED (video_arm() already brought
    // UVC up — fast path for racing-style triggers that need sub-second
    // start latency). Otherwise start UVC now.
    if (s_state == VIDEO_ARMED) {
        TLOG("already ARMED — skipping UVC bring-up");
    } else {
        TLOG("brl_uvc_start: calling");
        brl_uvc_set_resolution(s_res_index);
        if (!brl_uvc_start(on_uvc_frame)) {
            TLOG("brl_uvc_start: FAILED");
            log_e("UVC stream start failed");
            return false;
        }
        TLOG("brl_uvc_start: OK");
    }

    // Now mic_init — audio rate/channels are compile-time constants, safe to
    // query even before first mic_init() succeeds.
    bool audio_ok = mic_init();
    uint32_t aud_rate = audio_ok ? mic_sample_rate() : 0;
    uint8_t  aud_ch   = audio_ok ? mic_channels()    : 0;

    // Open AVI file. Lap-aware naming so each lap lands in its own file and
    // the Android app can download per-lap via GET /video/<stem>.
    char path[80];
    char stem[64];
    build_rec_path(lap_hint, path, sizeof(path), stem, sizeof(stem));
    TLOG("avi_writer_open: calling (f_expand 500 MB)");
    if (!avi_writer_open(path, res.width, res.height, res.fps,
                         aud_rate, aud_ch, 16)) {
        TLOG("avi_writer_open: FAILED");
        log_e("Cannot open AVI file");
        brl_uvc_stop();
        s_state = VIDEO_IDLE;  // camera stopped, not armed anymore
        return false;
    }
    TLOG("avi_writer_open: OK");
    s_current_lap_no = lap_hint;
    strncpy(s_current_stem, stem, sizeof(s_current_stem) - 1);
    s_current_stem[sizeof(s_current_stem) - 1] = '\0';

    // Start audio capture. Stream buffer is persistent (64 KB in PSRAM,
    // lazily allocated on first start). audio_writer_task is created per
    // recording (3 KB stack in internal DRAM); check return so silent
    // failure on fragmented heap surfaces in the log.
    if (audio_ok) {
        if (!s_audio_sb) {
            s_audio_sb = xStreamBufferCreateWithCaps(AUDIO_SB_BYTES, 1, MALLOC_CAP_SPIRAM);
        }
        if (s_audio_sb) {
            xStreamBufferReset(s_audio_sb);
            s_audio_writer_run = true;
            // 3 KB stack — same reasoning as mic_task: smaller was
            // unstable, bigger fragments internal DRAM faster.
            BaseType_t tc = xTaskCreatePinnedToCore(
                audio_writer_task_fn, "aud_wr", 3072, nullptr, 3,
                &s_audio_writer_task, 0);
            if (tc != pdPASS) {
                log_e("xTaskCreate(aud_wr) FAILED — recording without audio");
                s_audio_writer_run = false;
            } else if (!mic_start(on_mic_chunk)) {
                log_w("Mic start failed — recording without audio");
                s_audio_writer_run = false;
            }
        }
    }

    TLOG("all subsystems up, flipping state to RECORDING");
    s_rec_start_ms = millis();
    s_state = VIDEO_RECORDING;
    g_state.video_recording = true;

    log_i("Recording started: %dx%d @ %d fps → %s",
          res.width, res.height, res.fps, path);
    TLOG("video_start_recording: done");
    #undef TLOG
    return true;
}

void video_stop_recording(void)
{
    if (s_state != VIDEO_RECORDING) return;

    brl_uvc_stop();
    mic_stop();
    s_audio_writer_run = false;
    for (int i = 0; i < 50 && s_audio_writer_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    // Stop video pipeline tasks (decoder + encoder) — drain both
    s_pipeline_run = false;
    for (int i = 0; i < 50 && (s_pipeline_task || s_encode_task); i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    // Close the AVI FIRST (under mutex), THEN discard the ring. Previous
    // design drained the ring into the still-open AVI, which took 3-4 s
    // at SD speed and produced a confusing second "Writer: 30 ok frames"
    // log line after the user already pressed Stop. The tail 0.3 s of
    // footage we give up is negligible (the session dropped ~1000 frames
    // from ring overflow anyway).
    uint32_t frames = avi_writer_frame_count();  // capture before close
    if (s_avi_mux) xSemaphoreTake(s_avi_mux, portMAX_DELAY);
    avi_writer_close();
    s_current_lap_no = 0;
    s_current_stem[0] = '\0';
    if (s_avi_mux) xSemaphoreGive(s_avi_mux);

    // Now drain the ring quickly. Writer sees avi_writer_is_open()==false
    // and just drops each item — no SD writes. Tight loop, done in <100 ms.
    if (s_video_ring) {
        size_t item_size;
        while (true) {
            void *item = xRingbufferReceive(s_video_ring, &item_size, 0);
            if (!item) break;
            vRingbufferReturnItem(s_video_ring, item);
        }
        if (s_video_ring_dropped > 0) {
            log_w("Passthrough ring dropped %lu frames this session",
                  (unsigned long)s_video_ring_dropped);
            s_video_ring_dropped = 0;
        }
    }

    s_state = VIDEO_IDLE;
    g_state.video_recording = false;

    uint32_t dur = (millis() - s_rec_start_ms) / 1000;
    log_i("Recording stopped: %lu seconds, %lu frames",
          (unsigned long)dur, (unsigned long)frames);
}

bool video_split_for_next_lap(uint8_t next_lap_no)
{
    if (s_state != VIDEO_RECORDING) return false;
    if (!s_avi_mux) return false;
    // Skip if already pointed at this lap (e.g. A-B pre-roll file is named
    // _lap1.avi and the first S/F crossing also asks for lap 1).
    if (s_current_lap_no == next_lap_no && avi_writer_is_open()) {
        log_i("split: lap %u already current, skipping", (unsigned)next_lap_no);
        return true;
    }

    // Grab resolution/fps/audio from the currently active UVC config so the
    // new file matches the old. Audio params from mic in case it's still on.
    UvcResolution res = s_resolutions[s_res_index >= 0 ? s_res_index : 0];
    uint32_t aud_rate = mic_sample_rate();
    uint8_t  aud_ch   = mic_channels();

    char path[80];
    char stem[64];
    build_rec_path(next_lap_no, path, sizeof(path), stem, sizeof(stem));

    xSemaphoreTake(s_avi_mux, portMAX_DELAY);
    if (avi_writer_is_open()) avi_writer_close();
    bool ok = avi_writer_open(path, res.width, res.height, res.fps,
                              aud_rate, aud_ch, 16);
    if (ok) {
        s_current_lap_no = next_lap_no;
        strncpy(s_current_stem, stem, sizeof(s_current_stem) - 1);
        s_current_stem[sizeof(s_current_stem) - 1] = '\0';
    } else {
        s_current_lap_no = 0;
        s_current_stem[0] = '\0';
        log_e("split: avi_writer_open %s failed", path);
    }
    xSemaphoreGive(s_avi_mux);

    if (ok) log_i("split: -> %s (lap %u)", path, (unsigned)next_lap_no);
    return ok;
}

void video_get_current_filename_stem(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    if (!s_avi_mux || xSemaphoreTake(s_avi_mux, pdMS_TO_TICKS(100)) != pdTRUE) {
        buf[0] = '\0';
        return;
    }
    strncpy(buf, s_current_stem, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(s_avi_mux);
}

void video_start_preview(void)
{
    if (s_state == VIDEO_RECORDING) return;
    if (!brl_uvc_connected()) return;

    s_n_resolutions = brl_uvc_get_resolutions(s_resolutions, UVC_MAX_RESOLUTIONS);
    if (s_n_resolutions == 0) return;

    // For preview: pick lowest resolution (320x240)
    s_res_index = s_n_resolutions - 1;
    for (int i = 0; i < s_n_resolutions; i++) {
        if (s_resolutions[i].width <= 320) {
            s_res_index = i;
            break;
        }
    }

    const UvcResolution &res = s_resolutions[s_res_index];

    // Init HW JPEG pipeline for preview (decode MJPEG → RGB565 buffer).
    // RGB565 goes straight to LVGL image widget — no decode in render path.
    if (!s_pipeline_ready) {
        if (!video_pipeline_init(res.width, res.height)) {
            log_e("Pipeline init for preview failed");
            return;
        }
        s_pipeline_ready = true;
    }

    // Start UVC AFTER pipeline init: USB transfers are small (~7.5 KB) and
    // fit fine alongside the pipeline's DMA-aligned RGB buffers.
    brl_uvc_set_resolution(s_res_index);
    if (!brl_uvc_start(on_uvc_frame)) {
        log_e("UVC stream start failed");
        return;
    }

    s_state = VIDEO_PREVIEW;
    log_i("Preview started: %dx%d (HW decode → RGB565)", res.width, res.height);
}

void video_stop_preview(void)
{
    if (s_state != VIDEO_PREVIEW) return;
    brl_uvc_stop();
    s_pipeline_run = false;
    for (int i = 0; i < 50 && (s_pipeline_task || s_encode_task); i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    s_state = VIDEO_IDLE;
    log_i("Preview stopped");
}

VideoState video_get_state(void)
{
    return s_state;
}

bool video_camera_connected(void)
{
    return brl_uvc_connected();
}

int video_get_resolutions(VideoResolution *out, int max_count)
{
    s_n_resolutions = brl_uvc_get_resolutions(s_resolutions, UVC_MAX_RESOLUTIONS);
    int n = s_n_resolutions < max_count ? s_n_resolutions : max_count;
    for (int i = 0; i < n; i++) {
        out[i].width  = s_resolutions[i].width;
        out[i].height = s_resolutions[i].height;
        out[i].fps    = s_resolutions[i].fps;
    }
    return n;
}

void video_set_resolution(int index)
{
    if (index == s_res_index) return;
    s_res_index = index;
    brl_uvc_set_resolution(index);

    // Mark pipeline as needing re-init with new dimensions.
    // Actual buffer re-alloc happens in video_pipeline_init() which is called
    // from next video_start_preview() / video_start_recording().
    // HW JPEG codec engines stay alive (allocated at boot, DMA-clean).
    if (s_state == VIDEO_IDLE) {
        s_pipeline_ready = false;
        log_i("Resolution changed — pipeline will re-init on next start");
    }
}

int video_get_resolution_index(void)
{
    return s_res_index;
}

void video_set_quality(uint8_t quality)
{
    s_quality = quality;
    video_pipeline_set_quality(quality);
}

// HW-decoded RGB565 preview (from video_pipeline). LVGL renders this directly.
const uint8_t *video_get_preview_frame(uint16_t *width, uint16_t *height)
{
    return video_pipeline_get_preview(width, height);
}

// MJPEG path no longer used — preview goes via RGB565 for LVGL cache stability.
const uint8_t *video_get_preview_mjpeg(uint32_t *size_out)
{
    if (size_out) *size_out = 0;
    return nullptr;
}

uint32_t video_get_rec_duration_s(void)
{
    if (s_state != VIDEO_RECORDING) return 0;
    return (millis() - s_rec_start_ms) / 1000;
}

uint32_t video_get_rec_size_bytes(void)
{
    return avi_writer_file_size();
}

void video_set_passthrough(bool enabled)
{
    if (s_state == VIDEO_RECORDING) {
        log_w("Cannot change passthrough while recording");
        return;
    }
    s_passthrough = enabled;
    log_i("Passthrough mode: %s", enabled ? "ON (raw MJPEG)" : "OFF (on-device overlay)");
}

bool video_get_passthrough(void) { return s_passthrough; }

// ---------------------------------------------------------------------------
// Diagnostic: benchmark SD throughput while USB-ISO traffic is active.
// ---------------------------------------------------------------------------
static void sd_under_load_cb(const uint8_t * /*data*/, uint32_t /*size*/) {
    // No-op consumer. We don't care about frame data — we only need the
    // USB host to be pumping ISO transfers so SDMMC contends with them.
}

static void sd_under_load_task(void * /*arg*/) {
    log_i("[load-bench] waiting for camera...");
    for (int i = 0; i < 100 && !brl_uvc_connected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!brl_uvc_connected()) {
        log_w("[load-bench] no camera after 10 s — skipping");
        vTaskDelete(nullptr);
        return;
    }
    log_i("[load-bench] starting UVC stream...");
    if (!brl_uvc_start(sd_under_load_cb)) {
        log_w("[load-bench] brl_uvc_start failed — skipping");
        vTaskDelete(nullptr);
        return;
    }
    // Let the stream stabilise (camera AE/AWB + first transfers in-flight)
    vTaskDelay(pdMS_TO_TICKS(1500));
    log_i("[load-bench] USB stream active — running SD benchmark NOW");
    sd_mgr_benchmark();
    log_i("[load-bench] benchmark done — stopping UVC");
    brl_uvc_stop();
    vTaskDelete(nullptr);
}

void video_run_sd_under_load_benchmark(void)
{
    xTaskCreatePinnedToCore(sd_under_load_task, "sd_load_bench",
                            4096, nullptr, 3, nullptr, 0);
}
