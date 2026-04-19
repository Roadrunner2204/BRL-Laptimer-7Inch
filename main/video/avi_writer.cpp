/**
 * avi_writer.cpp -- Minimal AVI/RIFF container for MJPEG video + PCM audio
 *
 * Uses the native FatFS API (ff.h) directly instead of stdio. Pre-allocates
 * the file with f_expand() so streaming writes land in already-allocated
 * contiguous clusters without per-frame FAT-table updates. Measured 2026-04-18:
 * stdio path capped at ~500 KB/s on a Sandisk Extreme; native FatFS with
 * pre-allocation should lift that to multi-MB/s and let us drop the camera
 * bandwidth cap.
 *
 * File layout (when audio enabled):
 *   RIFF 'AVI '
 *     LIST 'hdrl'
 *       'avih'   MainAVIHeader                       (dwStreams = 2)
 *       LIST 'strl'                                   (stream 0: video)
 *         'strh'   AVIStreamHeader (vids/MJPG)
 *         'strf'   BITMAPINFOHEADER
 *       LIST 'strl'                                   (stream 1: audio)
 *         'strh'   AVIStreamHeader (auds/PCM)
 *         'strf'   WAVEFORMATEX
 *     LIST 'movi'
 *       '00dc' [size] [jpeg data]   (video frame)
 *       '01wb' [size] [pcm data]    (audio chunk)
 *       ...                          (interleaved)
 *     'idx1' [index entries]        (both video + audio)
 */

#include "avi_writer.h"
#include "../compat.h"
#include "ff.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "avi_writer";

// Pre-allocate this much contiguous space when opening a new AVI. Covers
// ~4 min of 2 MB/s recording; the unused tail is reclaimed via f_truncate
// when the file is closed. 500 MB on a 60 GB card is 1 %, harmless.
#define PREALLOC_BYTES ((FSIZE_t)(500ULL * 1024 * 1024))

// Internal-DMA chunk buffer for f_write. Source data comes from the
// video_mgr ring (PSRAM); a PSRAM pointer forces the SDMMC driver to
// allocate a per-write bounce buffer (slow, and fails with ESP_ERR_NO_MEM
// under DMA pressure). Staging through this internal-SRAM buffer lets
// SDMMC DMA directly — BUT only if the buffer is cache-line aligned.
// On ESP32-P4 the L2 cache line is 128 B (see CONFIG_CACHE_L2_CACHE_LINE_128B).
// Without alignment the driver falls back to its bounce-buffer path and
// calls esp_dma_capable_malloc() per write, which exhausts the internal
// DMA reserve and throws "dma_utils: esp_dma_capable_malloc: Not enough
// heap memory" → diskio_sdmmc returns 0x101 for every f_write.
// Size: 16 KB is a good balance — big enough to amortise f_write
// overhead (one large frame ≈ 3-4 writes), small enough to leave room
// for USB ISO transfers (~15 KB), mic I2S DMA descriptors (~16 KB),
// SDMMC driver state (~8 KB) and NimBLE/WiFi bits inside the 128 KB
// internal-DMA reserve. 48 KB was too big — a single allocation dragged
// ~50% of the reserve, leaving no contiguous room for the other
// consumers.
// 32 KB chunks amortise SDMMC command overhead (CMD25 multi-block
// write setup+teardown) over more sectors per f_write call. 16 KB
// capped effective write throughput at ~500 KB/s; 32 KB should lift
// that meaningfully. Still fits inside the 128 KB DMA reserve
// alongside USB ISO xfers (~15 KB), I2S DMA (~16 KB), SDMMC driver
// state (~8 KB), USB claim reserve (16 KB) — ~87 KB used, ~41 KB free.
#define AVI_CHUNK_SIZE   (16 * 1024)
#define AVI_CHUNK_ALIGN  128
static uint8_t *s_chunk_buf = nullptr;

// ---------------------------------------------------------------------------
// AVI structures (all little-endian)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

struct MainAVIHeader {
    uint32_t dwMicroSecPerFrame;
    uint32_t dwMaxBytesPerSec;
    uint32_t dwPaddingGranularity;
    uint32_t dwFlags;
    uint32_t dwTotalFrames;
    uint32_t dwInitialFrames;
    uint32_t dwStreams;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwWidth;
    uint32_t dwHeight;
    uint32_t dwReserved[4];
};

struct AVIStreamHeader {
    char     fccType[4];
    char     fccHandler[4];
    uint32_t dwFlags;
    uint16_t wPriority;
    uint16_t wLanguage;
    uint32_t dwInitialFrames;
    uint32_t dwScale;
    uint32_t dwRate;
    uint32_t dwStart;
    uint32_t dwLength;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwQuality;
    uint32_t dwSampleSize;
    int16_t  rcFrame[4];
};

struct BitmapInfoHeader {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    char     biCompression[4];
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

// WAVEFORMATEX (PCM subset, 18 bytes)
struct WaveFormatEx {
    uint16_t wFormatTag;       // 0x0001 = PCM
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;           // 0 for PCM
};

struct AVIIndexEntry {
    char     ckid[4];
    uint32_t dwFlags;
    uint32_t dwOffset;
    uint32_t dwSize;
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Writer state
// ---------------------------------------------------------------------------
static FIL       s_fil;
static bool      s_file_open = false;
static uint16_t  s_width = 0;
static uint16_t  s_height = 0;
static uint8_t   s_fps = 30;
static uint32_t  s_frame_count = 0;
static uint32_t  s_movi_start = 0;
static uint32_t  s_max_frame_size = 0;
static uint32_t  s_start_ms = 0;

// File offsets patched on close
static uint32_t  s_strh_vid_offset = 0;
static uint32_t  s_strh_aud_offset = 0;

// Audio
static bool      s_has_audio = false;
static uint32_t  s_audio_rate = 0;
static uint8_t   s_audio_channels = 0;
static uint8_t   s_audio_bits = 16;
static uint32_t  s_audio_bytes_total = 0;
static uint32_t  s_audio_samples_total = 0;  // per channel

// Dynamic index
static AVIIndexEntry *s_index = nullptr;
static uint32_t       s_index_count = 0;
static uint32_t       s_index_cap = 0;
static uint32_t       s_flush_counter = 0;

#define FLUSH_INTERVAL  150
#define INDEX_GROW_SIZE 2048

// ---------------------------------------------------------------------------
// Native-FatFS wrappers — thin shims so the rest of the code reads like
// the stdio version did.
// ---------------------------------------------------------------------------
static bool w_open(const char *path) {
    // VFS path "/sdcard/videos/..." → FatFS path "/videos/..." (uses the
    // default current drive, which is 0 on this board since sd_mgr does
    // the only fatfs mount).
    const char *fat_path = path;
    if (strncmp(path, "/sdcard", 7) == 0) fat_path = path + 7;
    FRESULT fr = f_open(&s_fil, fat_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        log_e("f_open(%s) = %d", fat_path, (int)fr);
        return false;
    }
    s_file_open = true;

#if FF_USE_EXPAND
    // Pre-allocate contiguous clusters so fwrites don't touch the FAT
    // per frame. opt=1 = immediately allocate, no zero-fill.
    fr = f_expand(&s_fil, PREALLOC_BYTES, 1);
    if (fr != FR_OK) {
        log_w("f_expand(%u MB) = %d — streaming will be slower",
              (unsigned)(PREALLOC_BYTES / (1024 * 1024)), (int)fr);
    }
    // Make sure we're at position 0 regardless of what f_expand did.
    f_lseek(&s_fil, 0);
#else
    log_w("FF_USE_EXPAND disabled in sdkconfig — streaming will be slower");
#endif
    return true;
}

static void w_close_truncated(uint32_t end_pos) {
    if (!s_file_open) return;
    // Truncate to end of actual data, freeing the rest of the 500 MB
    // pre-allocation. f_truncate cuts at the CURRENT file pointer.
    f_lseek(&s_fil, end_pos);
    f_truncate(&s_fil);
    f_close(&s_fil);
    s_file_open = false;
}

static size_t w_write(const void *data, size_t len) {
    if (!s_file_open) return 0;
    UINT bw = 0;
    if (f_write(&s_fil, data, len, &bw) != FR_OK) return 0;
    return bw;
}

// Chunked-copy write. Source may live in PSRAM (the video_mgr ring);
// we stage it through `s_chunk_buf` (internal DMA) so SDMMC can DMA
// straight out of the chunk buffer without falling back to its slow
// per-4KB transient copy path. Returns total bytes written.
// Diagnostic accumulators — summed across all chunks in a recording,
// logged by video_mgr every 30 frames alongside the existing stats.
static uint64_t s_memcpy_us   = 0;
static uint64_t s_fwrite_us   = 0;
static uint32_t s_chunk_count = 0;

void avi_writer_diag_snapshot(uint64_t *memcpy_us, uint64_t *fwrite_us, uint32_t *chunks) {
    if (memcpy_us) *memcpy_us = s_memcpy_us;
    if (fwrite_us) *fwrite_us = s_fwrite_us;
    if (chunks)    *chunks    = s_chunk_count;
    s_memcpy_us = 0; s_fwrite_us = 0; s_chunk_count = 0;
}

static size_t w_write_chunked(const void *data, size_t len) {
    if (!s_file_open) return 0;
    if (!s_chunk_buf) {
        // Fallback: direct write. Slow (500 KB/s) but correct.
        return w_write(data, len);
    }
    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = len;
    size_t total_written = 0;
    while (remaining > 0) {
        size_t chunk = remaining > AVI_CHUNK_SIZE ? AVI_CHUNK_SIZE : remaining;
        int64_t t0 = esp_timer_get_time();
        memcpy(s_chunk_buf, src, chunk);
        int64_t t1 = esp_timer_get_time();
        UINT bw = 0;
        FRESULT fr = f_write(&s_fil, s_chunk_buf, chunk, &bw);
        int64_t t2 = esp_timer_get_time();
        s_memcpy_us   += (uint64_t)(t1 - t0);
        s_fwrite_us   += (uint64_t)(t2 - t1);
        s_chunk_count += 1;
        if (fr != FR_OK) return total_written;
        total_written += bw;
        if (bw != chunk) return total_written;  // short write = error
        src += chunk;
        remaining -= chunk;
    }
    return total_written;
}

static bool w_seek(uint32_t pos) {
    if (!s_file_open) return false;
    return f_lseek(&s_fil, pos) == FR_OK;
}

static uint32_t w_tell(void) {
    return s_file_open ? (uint32_t)f_tell(&s_fil) : 0;
}

static void w_sync(void) {
    if (s_file_open) f_sync(&s_fil);
}

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------
static void write_fourcc(const char *cc) { w_write(cc, 4); }
static void write_u32(uint32_t v)        { w_write(&v, 4); }

static uint32_t write_list_start(const char *list_type, const char *sub_type) {
    write_fourcc(list_type);
    uint32_t size_pos = w_tell();
    write_u32(0);
    write_fourcc(sub_type);
    return size_pos;
}

static void write_list_end(uint32_t size_pos) {
    uint32_t end_pos = w_tell();
    uint32_t data_size = end_pos - size_pos - 4;
    w_seek(size_pos);
    write_u32(data_size);
    w_seek(end_pos);
}

static bool index_grow_if_needed(void) {
    if (s_index_count < s_index_cap) return true;
    uint32_t new_cap = s_index_cap + INDEX_GROW_SIZE;
    AVIIndexEntry *new_idx = (AVIIndexEntry *)heap_caps_realloc(
        s_index, new_cap * sizeof(AVIIndexEntry), MALLOC_CAP_SPIRAM);
    if (!new_idx) {
        log_e("Index realloc failed (%lu entries)", (unsigned long)new_cap);
        return false;
    }
    s_index = new_idx;
    s_index_cap = new_cap;
    return true;
}

static void flush_header(void) {
    if (!s_file_open) return;
    uint32_t cur_pos = w_tell();
    uint32_t riff_size = cur_pos - 8;
    w_seek(4);
    write_u32(riff_size);

    // avih.dwTotalFrames at offset 32 + 16 = 48
    w_seek(48);
    write_u32(s_frame_count);

    w_seek(cur_pos);
    w_sync();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool avi_writer_preallocate(void)
{
    if (s_chunk_buf) return true;
    // Cache-line aligned so SDMMC can DMA directly. Without this the
    // driver copies every block through a bounce buffer it allocates
    // fresh from the internal-DMA reserve — under the pressure of
    // USB + mic + LVGL, that alloc fails and all SD writes return
    // ESP_ERR_NO_MEM.
    s_chunk_buf = (uint8_t *)heap_caps_aligned_alloc(
        AVI_CHUNK_ALIGN, AVI_CHUNK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_chunk_buf) {
        log_i("AVI chunk buf: %u B @ %p (%d-aligned, DMA-direct)",
              (unsigned)AVI_CHUNK_SIZE, s_chunk_buf, AVI_CHUNK_ALIGN);
        return true;
    }
    log_e("AVI chunk buf alloc failed (%u B aligned %d, DMA) — SD writes will crawl",
          (unsigned)AVI_CHUNK_SIZE, AVI_CHUNK_ALIGN);
    return false;
}

bool avi_writer_open(const char *path, uint16_t width, uint16_t height, uint8_t fps,
                     uint32_t audio_rate, uint8_t audio_channels, uint8_t audio_bits)
{
    if (s_file_open) { log_e("AVI already open"); return false; }

    // Last-chance attempt in case video_init's preallocate() failed.
    if (!s_chunk_buf) avi_writer_preallocate();

    if (!w_open(path)) return false;

    s_width = width;
    s_height = height;
    s_fps = fps > 0 ? fps : 30;
    s_frame_count = 0;
    s_max_frame_size = 0;
    s_flush_counter = 0;
    s_start_ms = millis();

    s_has_audio = (audio_rate > 0 && audio_channels > 0);
    s_audio_rate = audio_rate;
    s_audio_channels = audio_channels;
    s_audio_bits = audio_bits ? audio_bits : 16;
    s_audio_bytes_total = 0;
    s_audio_samples_total = 0;

    // Allocate index
    s_index_cap = INDEX_GROW_SIZE;
    s_index_count = 0;
    s_index = (AVIIndexEntry *)heap_caps_malloc(
        s_index_cap * sizeof(AVIIndexEntry), MALLOC_CAP_SPIRAM);
    if (!s_index) {
        log_e("Index alloc failed");
        w_close_truncated(0);
        return false;
    }

    // ── RIFF + hdrl ──
    write_list_start("RIFF", "AVI ");
    uint32_t hdrl_size_pos = write_list_start("LIST", "hdrl");

    // avih
    write_fourcc("avih");
    write_u32(sizeof(MainAVIHeader));
    MainAVIHeader avih = {};
    avih.dwMicroSecPerFrame = 1000000 / s_fps;
    avih.dwMaxBytesPerSec = (uint32_t)width * height * 2;
    avih.dwFlags = 0x0010;  // AVIF_HASINDEX
    avih.dwTotalFrames = 0;
    avih.dwStreams = s_has_audio ? 2 : 1;
    avih.dwSuggestedBufferSize = width * height;
    avih.dwWidth = width;
    avih.dwHeight = height;
    w_write(&avih, sizeof(avih));

    // ── Stream 0: video ──
    uint32_t strl_v = write_list_start("LIST", "strl");
    write_fourcc("strh");
    write_u32(sizeof(AVIStreamHeader));
    s_strh_vid_offset = w_tell();
    AVIStreamHeader strh_v = {};
    memcpy(strh_v.fccType, "vids", 4);
    memcpy(strh_v.fccHandler, "MJPG", 4);
    strh_v.dwScale = 1000000 / s_fps;
    strh_v.dwRate  = 1000000;
    strh_v.dwSuggestedBufferSize = width * height;
    strh_v.dwQuality = 0xFFFFFFFF;
    w_write(&strh_v, sizeof(strh_v));

    write_fourcc("strf");
    write_u32(sizeof(BitmapInfoHeader));
    BitmapInfoHeader bih = {};
    bih.biSize = sizeof(BitmapInfoHeader);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    memcpy(bih.biCompression, "MJPG", 4);
    bih.biSizeImage = (uint32_t)width * height * 3;
    w_write(&bih, sizeof(bih));
    write_list_end(strl_v);

    // ── Stream 1: audio (optional) ──
    if (s_has_audio) {
        uint32_t block_align = (uint32_t)s_audio_channels * (s_audio_bits / 8);
        uint32_t avg_bps     = s_audio_rate * block_align;

        uint32_t strl_a = write_list_start("LIST", "strl");
        write_fourcc("strh");
        write_u32(sizeof(AVIStreamHeader));
        s_strh_aud_offset = w_tell();
        AVIStreamHeader strh_a = {};
        memcpy(strh_a.fccType, "auds", 4);
        // fccHandler = 0 for PCM
        strh_a.dwScale = block_align;      // bytes per sample-frame
        strh_a.dwRate  = avg_bps;          // bytes per second
        // dwRate/dwScale = samples/sec
        strh_a.dwSuggestedBufferSize = avg_bps / 4;  // ~250ms
        strh_a.dwQuality = 0xFFFFFFFF;
        strh_a.dwSampleSize = block_align;
        w_write(&strh_a, sizeof(strh_a));

        write_fourcc("strf");
        write_u32(sizeof(WaveFormatEx));
        WaveFormatEx wfx = {};
        wfx.wFormatTag = 0x0001;  // PCM
        wfx.nChannels = s_audio_channels;
        wfx.nSamplesPerSec = s_audio_rate;
        wfx.nAvgBytesPerSec = avg_bps;
        wfx.nBlockAlign = (uint16_t)block_align;
        wfx.wBitsPerSample = s_audio_bits;
        wfx.cbSize = 0;
        w_write(&wfx, sizeof(wfx));
        write_list_end(strl_a);
    }

    write_list_end(hdrl_size_pos);

    // ── movi ──
    write_fourcc("LIST");
    write_u32(0);
    write_fourcc("movi");
    s_movi_start = w_tell();

    log_i("AVI opened: %s (%dx%d @ %d fps, audio=%s)",
          path, width, height, s_fps,
          s_has_audio ? "on" : "off");
    return true;
}

bool avi_writer_write_frame(const uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!s_file_open || !jpeg_data || jpeg_size == 0) return false;
    if (!index_grow_if_needed()) return false;

    uint32_t chunk_offset = w_tell() - s_movi_start;

    write_fourcc("00dc");
    write_u32(jpeg_size);
    size_t written = w_write_chunked(jpeg_data, jpeg_size);
    if (written != jpeg_size) { log_e("SD write err"); return false; }
    if (jpeg_size & 1) { uint8_t pad = 0; w_write(&pad, 1); }

    AVIIndexEntry &idx = s_index[s_index_count++];
    memcpy(idx.ckid, "00dc", 4);
    idx.dwFlags = 0x10;  // KEYFRAME (every MJPEG is)
    idx.dwOffset = chunk_offset;
    idx.dwSize = jpeg_size;

    if (jpeg_size > s_max_frame_size) s_max_frame_size = jpeg_size;
    s_frame_count++;

    if (++s_flush_counter >= FLUSH_INTERVAL) {
        flush_header();
        s_flush_counter = 0;
    }
    return true;
}

bool avi_writer_write_audio(const int16_t *pcm, uint32_t sample_count)
{
    if (!s_file_open || !s_has_audio || !pcm || sample_count == 0) return false;
    if (!index_grow_if_needed()) return false;

    uint32_t bytes = sample_count * s_audio_channels * (s_audio_bits / 8);
    uint32_t chunk_offset = w_tell() - s_movi_start;

    write_fourcc("01wb");
    write_u32(bytes);
    size_t written = w_write_chunked(pcm, bytes);
    if (written != bytes) { log_e("SD audio write err"); return false; }
    if (bytes & 1) { uint8_t pad = 0; w_write(&pad, 1); }

    AVIIndexEntry &idx = s_index[s_index_count++];
    memcpy(idx.ckid, "01wb", 4);
    idx.dwFlags = 0x10;  // KEYFRAME-ish for PCM
    idx.dwOffset = chunk_offset;
    idx.dwSize = bytes;

    s_audio_bytes_total += bytes;
    s_audio_samples_total += sample_count;
    return true;
}

bool avi_writer_close(void)
{
    if (!s_file_open) return false;

    // ── Finalize movi ──
    uint32_t movi_end = w_tell();
    uint32_t movi_data_size = movi_end - s_movi_start + 4;
    w_seek(s_movi_start - 8);
    write_u32(movi_data_size);
    w_seek(movi_end);

    // ── idx1 ──
    write_fourcc("idx1");
    uint32_t idx_size = s_index_count * sizeof(AVIIndexEntry);
    write_u32(idx_size);
    if (s_index && s_index_count > 0) {
        w_write(s_index, idx_size);
    }

    // ── RIFF size ──
    uint32_t file_end = w_tell();   // end of actual AVI data
    w_seek(4);
    write_u32(file_end - 8);

    // ── Measured video fps ──
    uint32_t duration_ms = millis() - s_start_ms;
    uint64_t usec_per_frame = (s_frame_count > 0 && duration_ms > 0)
        ? ((uint64_t)duration_ms * 1000ULL) / s_frame_count
        : (uint64_t)(1000000 / s_fps);
    if (usec_per_frame == 0) usec_per_frame = 1000000 / s_fps;

    // Update avih
    w_seek(32);
    MainAVIHeader avih = {};
    avih.dwMicroSecPerFrame = (uint32_t)usec_per_frame;
    avih.dwMaxBytesPerSec = (duration_ms > 0)
        ? (uint32_t)((uint64_t)(movi_end - s_movi_start) * 1000ULL / duration_ms)
        : (s_max_frame_size * s_fps);
    avih.dwFlags = 0x0010;
    avih.dwTotalFrames = s_frame_count;
    avih.dwStreams = s_has_audio ? 2 : 1;
    avih.dwSuggestedBufferSize = s_max_frame_size;
    avih.dwWidth = s_width;
    avih.dwHeight = s_height;
    w_write(&avih, sizeof(avih));

    // Update video strh.dwScale/dwRate/dwLength
    if (s_strh_vid_offset > 0) {
        w_seek(s_strh_vid_offset + 20);
        write_u32((uint32_t)usec_per_frame); // dwScale
        write_u32(1000000);                   // dwRate
        w_seek(s_strh_vid_offset + 32);
        write_u32(s_frame_count);             // dwLength
    }

    // Update audio strh.dwLength (number of sample-frames)
    if (s_has_audio && s_strh_aud_offset > 0) {
        w_seek(s_strh_aud_offset + 32);
        write_u32(s_audio_samples_total);
    }

    // Close + truncate the pre-allocated tail back to the actual AVI
    // end. w_close_truncated seeks to file_end first, then f_truncate
    // releases everything beyond.
    w_close_truncated(file_end);

    if (s_index) { heap_caps_free(s_index); s_index = nullptr; }
    s_index_cap = 0;

    float fps_meas = (duration_ms > 0)
        ? (float)s_frame_count * 1000.0f / duration_ms : 0.0f;
    log_i("AVI closed: %lu frames / %lu audio samples, %lu B, %.2f fps",
          (unsigned long)s_frame_count,
          (unsigned long)s_audio_samples_total,
          (unsigned long)file_end, fps_meas);

    uint32_t saved = s_frame_count;
    s_frame_count = 0;
    s_index_count = 0;
    s_max_frame_size = 0;
    s_has_audio = false;
    return saved > 0;
}

bool     avi_writer_is_open(void)      { return s_file_open; }
uint32_t avi_writer_frame_count(void)  { return s_frame_count; }
uint32_t avi_writer_file_size(void)    { return s_file_open ? (uint32_t)f_size(&s_fil) : 0; }
