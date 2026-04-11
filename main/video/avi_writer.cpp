/**
 * avi_writer.cpp -- Minimal AVI/RIFF container for MJPEG video
 *
 * AVI structure:
 *   RIFF 'AVI '
 *     LIST 'hdrl'
 *       'avih'   MainAVIHeader
 *       LIST 'strl'
 *         'strh'   AVIStreamHeader (vids/MJPG)
 *         'strf'   BITMAPINFOHEADER
 *     LIST 'movi'
 *       '00dc' [size] [jpeg data]   (per frame)
 *       ...
 *     'idx1' [index entries]
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
    char     fccType[4];      // "vids"
    char     fccHandler[4];   // "MJPG"
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
    char     biCompression[4];  // "MJPG"
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

struct AVIIndexEntry {
    char     ckid[4];     // "00dc"
    uint32_t dwFlags;
    uint32_t dwOffset;    // offset from start of 'movi' list data
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
static uint32_t  s_movi_start = 0;  // file offset of 'movi' list data
static uint32_t  s_max_frame_size = 0;

// Index stored in PSRAM (grows dynamically)
static AVIIndexEntry *s_index = nullptr;
static uint32_t       s_index_cap = 0;
static uint32_t       s_flush_counter = 0;

#define FLUSH_INTERVAL  150   // update header every N frames (~5s at 30fps)
#define INDEX_GROW_SIZE 1024  // grow index by this many entries

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void write_fourcc(FILE *f, const char *cc) {
    fwrite(cc, 1, 4, f);
}

static void write_u32(FILE *f, uint32_t v) {
    fwrite(&v, 4, 1, f);
}

// Write RIFF/LIST header placeholder, return offset of size field
static uint32_t write_list_start(FILE *f, const char *list_type, const char *sub_type) {
    write_fourcc(f, list_type);
    uint32_t size_pos = (uint32_t)ftell(f);
    write_u32(f, 0);  // placeholder size
    write_fourcc(f, sub_type);
    return size_pos;
}

// Patch the size field at size_pos with (current_pos - size_pos - 4)
static void write_list_end(FILE *f, uint32_t size_pos) {
    uint32_t end_pos = (uint32_t)ftell(f);
    uint32_t data_size = end_pos - size_pos - 4;
    fseek(f, size_pos, SEEK_SET);
    write_u32(f, data_size);
    fseek(f, end_pos, SEEK_SET);
}

// Update the main RIFF size and avih.dwTotalFrames without closing
static void flush_header(void) {
    if (!s_file) return;

    uint32_t cur_pos = (uint32_t)ftell(s_file);

    // Update RIFF size (offset 4)
    uint32_t riff_size = cur_pos - 8;
    fseek(s_file, 4, SEEK_SET);
    write_u32(s_file, riff_size);

    // Update avih.dwTotalFrames (offset 48 = RIFF(12) + LIST_hdrl(12) + avih_tag(8) + 16)
    //   RIFF(4+4+4) + LIST(4+4+4) + 'avih'(4) + size(4) + 16 bytes into struct = offset 48
    fseek(s_file, 48, SEEK_SET);
    write_u32(s_file, s_frame_count);

    // Update strh.dwLength
    // hdrl_start = 12, LIST_strl starts after avih chunk
    // avih chunk = 8 + 56 = 64 bytes from hdrl data start
    // LIST_strl = 12 bytes header, then strh tag(4) + size(4) + ... dwLength at offset +20
    // Exact offset computed during open; for simplicity, store it
    // (We handle this in close() instead — periodic flush updates RIFF + frame count only)

    fseek(s_file, cur_pos, SEEK_SET);
    fflush(s_file);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool avi_writer_open(const char *path, uint16_t width, uint16_t height, uint8_t fps)
{
    if (s_file) {
        log_e("AVI file already open");
        return false;
    }

    s_file = fopen(path, "wb");
    if (!s_file) {
        log_e("Cannot create %s", path);
        return false;
    }

    s_width = width;
    s_height = height;
    s_fps = fps > 0 ? fps : 30;
    s_frame_count = 0;
    s_max_frame_size = 0;
    s_flush_counter = 0;

    // Allocate index in PSRAM
    s_index_cap = INDEX_GROW_SIZE;
    s_index = (AVIIndexEntry *)heap_caps_malloc(
        s_index_cap * sizeof(AVIIndexEntry), MALLOC_CAP_SPIRAM);
    if (!s_index) {
        log_e("Index alloc failed");
        fclose(s_file);
        s_file = nullptr;
        return false;
    }

    // ── RIFF header ──
    uint32_t riff_size_pos = write_list_start(s_file, "RIFF", "AVI ");

    // ── LIST 'hdrl' ──
    uint32_t hdrl_size_pos = write_list_start(s_file, "LIST", "hdrl");

    // ── avih (Main AVI Header) ──
    write_fourcc(s_file, "avih");
    write_u32(s_file, sizeof(MainAVIHeader));

    MainAVIHeader avih = {};
    avih.dwMicroSecPerFrame = 1000000 / s_fps;
    avih.dwMaxBytesPerSec = width * height * 2;  // estimate
    avih.dwFlags = 0x0010;  // AVIF_HASINDEX
    avih.dwTotalFrames = 0;  // updated on close
    avih.dwStreams = 1;
    avih.dwSuggestedBufferSize = width * height;
    avih.dwWidth = width;
    avih.dwHeight = height;
    fwrite(&avih, sizeof(avih), 1, s_file);

    // ── LIST 'strl' ──
    uint32_t strl_size_pos = write_list_start(s_file, "LIST", "strl");

    // ── strh (Stream Header) ──
    write_fourcc(s_file, "strh");
    write_u32(s_file, sizeof(AVIStreamHeader));

    AVIStreamHeader strh = {};
    memcpy(strh.fccType, "vids", 4);
    memcpy(strh.fccHandler, "MJPG", 4);
    strh.dwScale = 1;
    strh.dwRate = s_fps;
    strh.dwLength = 0;  // updated on close
    strh.dwSuggestedBufferSize = width * height;
    strh.dwQuality = 0xFFFFFFFF;  // default
    fwrite(&strh, sizeof(strh), 1, s_file);

    // ── strf (Stream Format — BITMAPINFOHEADER) ──
    write_fourcc(s_file, "strf");
    write_u32(s_file, sizeof(BitmapInfoHeader));

    BitmapInfoHeader bih = {};
    bih.biSize = sizeof(BitmapInfoHeader);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    memcpy(bih.biCompression, "MJPG", 4);
    bih.biSizeImage = width * height * 3;
    fwrite(&bih, sizeof(bih), 1, s_file);

    write_list_end(s_file, strl_size_pos);
    write_list_end(s_file, hdrl_size_pos);

    // ── LIST 'movi' ──
    write_fourcc(s_file, "LIST");
    // movi size placeholder — we'll update on close
    write_u32(s_file, 0);
    write_fourcc(s_file, "movi");
    s_movi_start = (uint32_t)ftell(s_file);

    log_i("AVI opened: %s (%dx%d @ %d fps)", path, width, height, s_fps);
    return true;
}

bool avi_writer_write_frame(const uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!s_file || !jpeg_data || jpeg_size == 0) return false;

    // Grow index if needed
    if (s_frame_count >= s_index_cap) {
        uint32_t new_cap = s_index_cap + INDEX_GROW_SIZE;
        AVIIndexEntry *new_idx = (AVIIndexEntry *)heap_caps_realloc(
            s_index, new_cap * sizeof(AVIIndexEntry), MALLOC_CAP_SPIRAM);
        if (!new_idx) {
            log_e("Index realloc failed at frame %lu", (unsigned long)s_frame_count);
            return false;
        }
        s_index = new_idx;
        s_index_cap = new_cap;
    }

    uint32_t chunk_offset = (uint32_t)ftell(s_file) - s_movi_start;

    // Write '00dc' chunk
    write_fourcc(s_file, "00dc");
    write_u32(s_file, jpeg_size);
    size_t written = fwrite(jpeg_data, 1, jpeg_size, s_file);
    if (written != jpeg_size) {
        log_e("SD write error: wrote %u/%lu", (unsigned)written, (unsigned long)jpeg_size);
        return false;
    }

    // Pad to 2-byte boundary (AVI requirement)
    if (jpeg_size & 1) {
        uint8_t pad = 0;
        fwrite(&pad, 1, 1, s_file);
    }

    // Add index entry
    AVIIndexEntry &idx = s_index[s_frame_count];
    memcpy(idx.ckid, "00dc", 4);
    idx.dwFlags = 0x10;  // AVIIF_KEYFRAME (all MJPEG frames are keyframes)
    idx.dwOffset = chunk_offset;
    idx.dwSize = jpeg_size;

    if (jpeg_size > s_max_frame_size) s_max_frame_size = jpeg_size;
    s_frame_count++;

    // Periodic header flush for crash safety
    if (++s_flush_counter >= FLUSH_INTERVAL) {
        flush_header();
        s_flush_counter = 0;
    }

    return true;
}

bool avi_writer_close(void)
{
    if (!s_file) return false;

    // ── Finalize movi list size ──
    uint32_t movi_end = (uint32_t)ftell(s_file);
    uint32_t movi_data_size = movi_end - s_movi_start + 4;  // +4 for 'movi' fourcc
    // movi LIST size is at s_movi_start - 8 (LIST fourcc) + 4 (size field)
    fseek(s_file, s_movi_start - 8, SEEK_SET);
    write_u32(s_file, movi_data_size);
    fseek(s_file, movi_end, SEEK_SET);

    // ── Write idx1 index ──
    write_fourcc(s_file, "idx1");
    uint32_t idx_size = s_frame_count * sizeof(AVIIndexEntry);
    write_u32(s_file, idx_size);
    if (s_index && s_frame_count > 0) {
        fwrite(s_index, sizeof(AVIIndexEntry), s_frame_count, s_file);
    }

    // ── Update RIFF size ──
    uint32_t file_end = (uint32_t)ftell(s_file);
    fseek(s_file, 4, SEEK_SET);
    write_u32(s_file, file_end - 8);

    // ── Update avih ──
    // avih starts at offset 32 (RIFF:12 + LIST_hdrl:12 + 'avih':4 + size:4)
    fseek(s_file, 32, SEEK_SET);
    MainAVIHeader avih = {};
    avih.dwMicroSecPerFrame = 1000000 / s_fps;
    avih.dwMaxBytesPerSec = s_max_frame_size * s_fps;
    avih.dwFlags = 0x0010;  // AVIF_HASINDEX
    avih.dwTotalFrames = s_frame_count;
    avih.dwStreams = 1;
    avih.dwSuggestedBufferSize = s_max_frame_size;
    avih.dwWidth = s_width;
    avih.dwHeight = s_height;
    fwrite(&avih, sizeof(avih), 1, s_file);

    // ── Update strh.dwLength ──
    // strh starts after avih: offset 32 + 56 (avih data) + LIST_strl:12 + 'strh':4 + size:4
    // = 32 + 56 + 12 + 8 = 108
    // dwLength is at offset 20 within strh struct
    fseek(s_file, 108 + 20, SEEK_SET);
    write_u32(s_file, s_frame_count);

    fclose(s_file);
    s_file = nullptr;

    // Free index
    if (s_index) {
        heap_caps_free(s_index);
        s_index = nullptr;
    }
    s_index_cap = 0;

    log_i("AVI closed: %lu frames, %lu bytes",
          (unsigned long)s_frame_count, (unsigned long)file_end);

    uint32_t saved_count = s_frame_count;
    s_frame_count = 0;
    s_max_frame_size = 0;
    return saved_count > 0;
}

bool avi_writer_is_open(void) {
    return s_file != nullptr;
}

uint32_t avi_writer_frame_count(void) {
    return s_frame_count;
}

uint32_t avi_writer_file_size(void) {
    if (!s_file) return 0;
    return (uint32_t)ftell(s_file);
}
