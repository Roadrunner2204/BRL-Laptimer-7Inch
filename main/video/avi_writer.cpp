/**
 * avi_writer.cpp -- Minimal AVI/RIFF container for MJPEG video + PCM audio
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
#include <string.h>
#include <stdlib.h>

static const char *TAG = "avi_writer";

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
static FILE     *s_file = nullptr;
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
// Low-level helpers
// ---------------------------------------------------------------------------
static void write_fourcc(FILE *f, const char *cc) { fwrite(cc, 1, 4, f); }
static void write_u32(FILE *f, uint32_t v)        { fwrite(&v, 4, 1, f); }

static uint32_t write_list_start(FILE *f, const char *list_type, const char *sub_type) {
    write_fourcc(f, list_type);
    uint32_t size_pos = (uint32_t)ftell(f);
    write_u32(f, 0);
    write_fourcc(f, sub_type);
    return size_pos;
}

static void write_list_end(FILE *f, uint32_t size_pos) {
    uint32_t end_pos = (uint32_t)ftell(f);
    uint32_t data_size = end_pos - size_pos - 4;
    fseek(f, size_pos, SEEK_SET);
    write_u32(f, data_size);
    fseek(f, end_pos, SEEK_SET);
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
    if (!s_file) return;
    uint32_t cur_pos = (uint32_t)ftell(s_file);
    uint32_t riff_size = cur_pos - 8;
    fseek(s_file, 4, SEEK_SET);
    write_u32(s_file, riff_size);

    // avih.dwTotalFrames at offset 32 + 16 = 48
    fseek(s_file, 48, SEEK_SET);
    write_u32(s_file, s_frame_count);

    fseek(s_file, cur_pos, SEEK_SET);
    fflush(s_file);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool avi_writer_open(const char *path, uint16_t width, uint16_t height, uint8_t fps,
                     uint32_t audio_rate, uint8_t audio_channels, uint8_t audio_bits)
{
    if (s_file) { log_e("AVI already open"); return false; }

    s_file = fopen(path, "wb");
    if (!s_file) { log_e("Cannot create %s", path); return false; }

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
    if (!s_index) { log_e("Index alloc failed"); fclose(s_file); s_file = nullptr; return false; }

    // ── RIFF + hdrl ──
    write_list_start(s_file, "RIFF", "AVI ");
    uint32_t hdrl_size_pos = write_list_start(s_file, "LIST", "hdrl");

    // avih
    write_fourcc(s_file, "avih");
    write_u32(s_file, sizeof(MainAVIHeader));
    MainAVIHeader avih = {};
    avih.dwMicroSecPerFrame = 1000000 / s_fps;
    avih.dwMaxBytesPerSec = (uint32_t)width * height * 2;
    avih.dwFlags = 0x0010;  // AVIF_HASINDEX
    avih.dwTotalFrames = 0;
    avih.dwStreams = s_has_audio ? 2 : 1;
    avih.dwSuggestedBufferSize = width * height;
    avih.dwWidth = width;
    avih.dwHeight = height;
    fwrite(&avih, sizeof(avih), 1, s_file);

    // ── Stream 0: video ──
    uint32_t strl_v = write_list_start(s_file, "LIST", "strl");
    write_fourcc(s_file, "strh");
    write_u32(s_file, sizeof(AVIStreamHeader));
    s_strh_vid_offset = (uint32_t)ftell(s_file);
    AVIStreamHeader strh_v = {};
    memcpy(strh_v.fccType, "vids", 4);
    memcpy(strh_v.fccHandler, "MJPG", 4);
    strh_v.dwScale = 1000000 / s_fps;
    strh_v.dwRate  = 1000000;
    strh_v.dwSuggestedBufferSize = width * height;
    strh_v.dwQuality = 0xFFFFFFFF;
    fwrite(&strh_v, sizeof(strh_v), 1, s_file);

    write_fourcc(s_file, "strf");
    write_u32(s_file, sizeof(BitmapInfoHeader));
    BitmapInfoHeader bih = {};
    bih.biSize = sizeof(BitmapInfoHeader);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    memcpy(bih.biCompression, "MJPG", 4);
    bih.biSizeImage = (uint32_t)width * height * 3;
    fwrite(&bih, sizeof(bih), 1, s_file);
    write_list_end(s_file, strl_v);

    // ── Stream 1: audio (optional) ──
    if (s_has_audio) {
        uint32_t block_align = (uint32_t)s_audio_channels * (s_audio_bits / 8);
        uint32_t avg_bps     = s_audio_rate * block_align;

        uint32_t strl_a = write_list_start(s_file, "LIST", "strl");
        write_fourcc(s_file, "strh");
        write_u32(s_file, sizeof(AVIStreamHeader));
        s_strh_aud_offset = (uint32_t)ftell(s_file);
        AVIStreamHeader strh_a = {};
        memcpy(strh_a.fccType, "auds", 4);
        // fccHandler = 0 for PCM
        strh_a.dwScale = block_align;      // bytes per sample-frame
        strh_a.dwRate  = avg_bps;          // bytes per second
        // dwRate/dwScale = samples/sec
        strh_a.dwSuggestedBufferSize = avg_bps / 4;  // ~250ms
        strh_a.dwQuality = 0xFFFFFFFF;
        strh_a.dwSampleSize = block_align;
        fwrite(&strh_a, sizeof(strh_a), 1, s_file);

        write_fourcc(s_file, "strf");
        write_u32(s_file, sizeof(WaveFormatEx));
        WaveFormatEx wfx = {};
        wfx.wFormatTag = 0x0001;  // PCM
        wfx.nChannels = s_audio_channels;
        wfx.nSamplesPerSec = s_audio_rate;
        wfx.nAvgBytesPerSec = avg_bps;
        wfx.nBlockAlign = (uint16_t)block_align;
        wfx.wBitsPerSample = s_audio_bits;
        wfx.cbSize = 0;
        fwrite(&wfx, sizeof(wfx), 1, s_file);
        write_list_end(s_file, strl_a);
    }

    write_list_end(s_file, hdrl_size_pos);

    // ── movi ──
    write_fourcc(s_file, "LIST");
    write_u32(s_file, 0);
    write_fourcc(s_file, "movi");
    s_movi_start = (uint32_t)ftell(s_file);

    log_i("AVI opened: %s (%dx%d @ %d fps, audio=%s)",
          path, width, height, s_fps,
          s_has_audio ? "on" : "off");
    return true;
}

bool avi_writer_write_frame(const uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!s_file || !jpeg_data || jpeg_size == 0) return false;
    if (!index_grow_if_needed()) return false;

    uint32_t chunk_offset = (uint32_t)ftell(s_file) - s_movi_start;

    write_fourcc(s_file, "00dc");
    write_u32(s_file, jpeg_size);
    size_t written = fwrite(jpeg_data, 1, jpeg_size, s_file);
    if (written != jpeg_size) { log_e("SD write err"); return false; }
    if (jpeg_size & 1) { uint8_t pad = 0; fwrite(&pad, 1, 1, s_file); }

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
    if (!s_file || !s_has_audio || !pcm || sample_count == 0) return false;
    if (!index_grow_if_needed()) return false;

    uint32_t bytes = sample_count * s_audio_channels * (s_audio_bits / 8);
    uint32_t chunk_offset = (uint32_t)ftell(s_file) - s_movi_start;

    write_fourcc(s_file, "01wb");
    write_u32(s_file, bytes);
    size_t written = fwrite(pcm, 1, bytes, s_file);
    if (written != bytes) { log_e("SD audio write err"); return false; }
    if (bytes & 1) { uint8_t pad = 0; fwrite(&pad, 1, 1, s_file); }

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
    if (!s_file) return false;

    // ── Finalize movi ──
    uint32_t movi_end = (uint32_t)ftell(s_file);
    uint32_t movi_data_size = movi_end - s_movi_start + 4;
    fseek(s_file, s_movi_start - 8, SEEK_SET);
    write_u32(s_file, movi_data_size);
    fseek(s_file, movi_end, SEEK_SET);

    // ── idx1 ──
    write_fourcc(s_file, "idx1");
    uint32_t idx_size = s_index_count * sizeof(AVIIndexEntry);
    write_u32(s_file, idx_size);
    if (s_index && s_index_count > 0) {
        fwrite(s_index, sizeof(AVIIndexEntry), s_index_count, s_file);
    }

    // ── RIFF size ──
    uint32_t file_end = (uint32_t)ftell(s_file);
    fseek(s_file, 4, SEEK_SET);
    write_u32(s_file, file_end - 8);

    // ── Measured video fps ──
    uint32_t duration_ms = millis() - s_start_ms;
    uint64_t usec_per_frame = (s_frame_count > 0 && duration_ms > 0)
        ? ((uint64_t)duration_ms * 1000ULL) / s_frame_count
        : (uint64_t)(1000000 / s_fps);
    if (usec_per_frame == 0) usec_per_frame = 1000000 / s_fps;

    // Update avih
    fseek(s_file, 32, SEEK_SET);
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
    fwrite(&avih, sizeof(avih), 1, s_file);

    // Update video strh.dwScale/dwRate/dwLength
    if (s_strh_vid_offset > 0) {
        fseek(s_file, s_strh_vid_offset + 20, SEEK_SET);
        write_u32(s_file, (uint32_t)usec_per_frame); // dwScale
        write_u32(s_file, 1000000);                   // dwRate
        fseek(s_file, s_strh_vid_offset + 32, SEEK_SET);
        write_u32(s_file, s_frame_count);             // dwLength
    }

    // Update audio strh.dwLength (number of sample-frames)
    if (s_has_audio && s_strh_aud_offset > 0) {
        fseek(s_file, s_strh_aud_offset + 32, SEEK_SET);
        write_u32(s_file, s_audio_samples_total);
    }

    fclose(s_file);
    s_file = nullptr;
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

bool     avi_writer_is_open(void)      { return s_file != nullptr; }
uint32_t avi_writer_frame_count(void)  { return s_frame_count; }
uint32_t avi_writer_file_size(void)    { return s_file ? (uint32_t)ftell(s_file) : 0; }
