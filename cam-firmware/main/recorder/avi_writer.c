/**
 * avi_writer.c — MJPEG/AVI container writer for the BRL cam module.
 *
 * Slimmed-down recycle of the laptimer's pre-removal avi_writer.cpp
 * (commit e08cb92~1, BRL-Laptimer main repo). Audio paths and the
 * throughput-debug snapshot helper are dropped — cam module has no mic
 * and no longer competes with USB-ISO for SDMMC DMA.
 *
 * Performance notes carried over verbatim, since they still apply:
 *   • Native FatFS (ff.h) instead of stdio: skips a per-call mutex and
 *     gives us f_expand for contiguous pre-allocation.
 *   • PREALLOC_BYTES = 500 MB: ~4 min of 2 MB/s recording. f_truncate
 *     reclaims the unused tail on close.
 *   • Chunk buffer in 128 B-aligned MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA
 *     so SDMMC DMA reads straight from it. Without alignment the
 *     driver allocates a transient bounce buffer per write and chokes.
 *   • 16 KB chunk size: amortises CMD25 overhead while staying well
 *     below the internal-DMA reserve so other DMA consumers (SDMMC
 *     state, esp_hosted SDIO, …) keep their share.
 */

#include "avi_writer.h"
#include "ff.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "avi_writer";

#define PREALLOC_BYTES   ((FSIZE_t)(500ULL * 1024 * 1024))
#define AVI_CHUNK_SIZE   (16 * 1024)
#define AVI_CHUNK_ALIGN  128
#define FLUSH_INTERVAL   150
#define INDEX_GROW_SIZE  2048

static uint8_t *s_chunk_buf = NULL;

#pragma pack(push, 1)
typedef struct {
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
} MainAVIHeader;

typedef struct {
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
} AVIStreamHeader;

typedef struct {
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
} BitmapInfoHeader;

typedef struct {
    char     ckid[4];
    uint32_t dwFlags;
    uint32_t dwOffset;
    uint32_t dwSize;
} AVIIndexEntry;
#pragma pack(pop)

static FIL       s_fil;
static bool      s_file_open = false;
static uint16_t  s_width = 0;
static uint16_t  s_height = 0;
static uint8_t   s_fps = 30;
static uint32_t  s_frame_count = 0;
static uint32_t  s_movi_start = 0;
static uint32_t  s_max_frame_size = 0;
static uint32_t  s_start_ms = 0;
static uint32_t  s_strh_vid_offset = 0;

static AVIIndexEntry *s_index = NULL;
static uint32_t       s_index_count = 0;
static uint32_t       s_index_cap = 0;
static uint32_t       s_flush_counter = 0;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ── Native-FatFS wrappers ──────────────────────────────────────────── */
static bool w_open(const char *path)
{
    /* "/sdcard/foo" → "/foo" — FatFS uses the default current drive. */
    const char *fat_path = path;
    if (strncmp(path, "/sdcard", 7) == 0) fat_path = path + 7;
    FRESULT fr = f_open(&s_fil, fat_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_open(%s) = %d", fat_path, (int)fr);
        return false;
    }
    s_file_open = true;
#if FF_USE_EXPAND
    fr = f_expand(&s_fil, PREALLOC_BYTES, 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "f_expand(%u MB) = %d — streaming will be slower",
                 (unsigned)(PREALLOC_BYTES / (1024 * 1024)), (int)fr);
    }
    f_lseek(&s_fil, 0);
#else
    ESP_LOGW(TAG, "FF_USE_EXPAND disabled in sdkconfig — streaming will be slower");
#endif
    return true;
}

static void w_close_truncated(uint32_t end_pos)
{
    if (!s_file_open) return;
    f_lseek(&s_fil, end_pos);
    f_truncate(&s_fil);
    f_close(&s_fil);
    s_file_open = false;
}

static size_t w_write(const void *data, size_t len)
{
    if (!s_file_open) return 0;
    UINT bw = 0;
    if (f_write(&s_fil, data, len, &bw) != FR_OK) return 0;
    return bw;
}

static size_t w_write_chunked(const void *data, size_t len)
{
    if (!s_file_open) return 0;
    if (!s_chunk_buf) return w_write(data, len);    /* slow fallback */

    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = len;
    size_t total = 0;
    while (remaining > 0) {
        size_t chunk = remaining > AVI_CHUNK_SIZE ? AVI_CHUNK_SIZE : remaining;
        memcpy(s_chunk_buf, src, chunk);
        UINT bw = 0;
        FRESULT fr = f_write(&s_fil, s_chunk_buf, chunk, &bw);
        if (fr != FR_OK) return total;
        total += bw;
        if (bw != chunk) return total;
        src += chunk;
        remaining -= chunk;
    }
    return total;
}

static bool w_seek(uint32_t pos)
{
    if (!s_file_open) return false;
    return f_lseek(&s_fil, pos) == FR_OK;
}

static uint32_t w_tell(void) { return s_file_open ? (uint32_t)f_tell(&s_fil) : 0; }
static void w_sync(void)     { if (s_file_open) f_sync(&s_fil); }

/* ── Low-level helpers ──────────────────────────────────────────────── */
static void write_fourcc(const char *cc) { w_write(cc, 4); }
static void write_u32(uint32_t v)        { w_write(&v, 4); }

static uint32_t write_list_start(const char *list_type, const char *sub_type)
{
    write_fourcc(list_type);
    uint32_t size_pos = w_tell();
    write_u32(0);
    write_fourcc(sub_type);
    return size_pos;
}

static void write_list_end(uint32_t size_pos)
{
    uint32_t end_pos = w_tell();
    uint32_t data_size = end_pos - size_pos - 4;
    w_seek(size_pos);
    write_u32(data_size);
    w_seek(end_pos);
}

static bool index_grow_if_needed(void)
{
    if (s_index_count < s_index_cap) return true;
    uint32_t new_cap = s_index_cap + INDEX_GROW_SIZE;
    AVIIndexEntry *new_idx = (AVIIndexEntry *)heap_caps_realloc(
        s_index, new_cap * sizeof(AVIIndexEntry), MALLOC_CAP_SPIRAM);
    if (!new_idx) {
        ESP_LOGE(TAG, "Index realloc failed (%lu entries)", (unsigned long)new_cap);
        return false;
    }
    s_index = new_idx;
    s_index_cap = new_cap;
    return true;
}

static void flush_header(void)
{
    if (!s_file_open) return;
    uint32_t cur_pos = w_tell();
    uint32_t riff_size = cur_pos - 8;
    w_seek(4);
    write_u32(riff_size);
    /* avih.dwTotalFrames at offset 32 + 16 = 48 */
    w_seek(48);
    write_u32(s_frame_count);
    w_seek(cur_pos);
    w_sync();
}

/* ── Public API ─────────────────────────────────────────────────────── */
bool avi_writer_preallocate(void)
{
    if (s_chunk_buf) return true;
    s_chunk_buf = (uint8_t *)heap_caps_aligned_alloc(
        AVI_CHUNK_ALIGN, AVI_CHUNK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_chunk_buf) {
        ESP_LOGI(TAG, "AVI chunk buf: %u B @ %p (%d-aligned, DMA-direct)",
                 (unsigned)AVI_CHUNK_SIZE, s_chunk_buf, AVI_CHUNK_ALIGN);
        return true;
    }
    ESP_LOGE(TAG, "AVI chunk buf alloc failed (%u B aligned %d, DMA)",
             (unsigned)AVI_CHUNK_SIZE, AVI_CHUNK_ALIGN);
    return false;
}

bool avi_writer_open(const char *path, uint16_t width, uint16_t height, uint8_t fps)
{
    if (s_file_open) { ESP_LOGE(TAG, "AVI already open"); return false; }
    if (!s_chunk_buf) avi_writer_preallocate();
    if (!w_open(path)) return false;

    s_width = width;
    s_height = height;
    s_fps = fps > 0 ? fps : 30;
    s_frame_count = 0;
    s_max_frame_size = 0;
    s_flush_counter = 0;
    s_start_ms = now_ms();
    s_strh_vid_offset = 0;

    s_index_cap = INDEX_GROW_SIZE;
    s_index_count = 0;
    s_index = (AVIIndexEntry *)heap_caps_malloc(
        s_index_cap * sizeof(AVIIndexEntry), MALLOC_CAP_SPIRAM);
    if (!s_index) {
        ESP_LOGE(TAG, "Index alloc failed");
        w_close_truncated(0);
        return false;
    }

    /* RIFF + hdrl */
    write_list_start("RIFF", "AVI ");
    uint32_t hdrl_size_pos = write_list_start("LIST", "hdrl");

    /* avih */
    write_fourcc("avih");
    write_u32(sizeof(MainAVIHeader));
    MainAVIHeader avih = {0};
    avih.dwMicroSecPerFrame = 1000000 / s_fps;
    avih.dwMaxBytesPerSec   = (uint32_t)width * height * 2;
    avih.dwFlags            = 0x0010;       /* AVIF_HASINDEX */
    avih.dwTotalFrames      = 0;
    avih.dwStreams          = 1;
    avih.dwSuggestedBufferSize = (uint32_t)width * height;
    avih.dwWidth            = width;
    avih.dwHeight           = height;
    w_write(&avih, sizeof(avih));

    /* Stream 0: video */
    uint32_t strl_v = write_list_start("LIST", "strl");
    write_fourcc("strh");
    write_u32(sizeof(AVIStreamHeader));
    s_strh_vid_offset = w_tell();
    AVIStreamHeader strh_v = {0};
    memcpy(strh_v.fccType, "vids", 4);
    memcpy(strh_v.fccHandler, "MJPG", 4);
    strh_v.dwScale = 1000000 / s_fps;
    strh_v.dwRate  = 1000000;
    strh_v.dwSuggestedBufferSize = (uint32_t)width * height;
    strh_v.dwQuality = 0xFFFFFFFF;
    w_write(&strh_v, sizeof(strh_v));

    write_fourcc("strf");
    write_u32(sizeof(BitmapInfoHeader));
    BitmapInfoHeader bih = {0};
    bih.biSize     = sizeof(BitmapInfoHeader);
    bih.biWidth    = width;
    bih.biHeight   = height;
    bih.biPlanes   = 1;
    bih.biBitCount = 24;
    memcpy(bih.biCompression, "MJPG", 4);
    bih.biSizeImage = (uint32_t)width * height * 3;
    w_write(&bih, sizeof(bih));
    write_list_end(strl_v);

    write_list_end(hdrl_size_pos);

    /* movi */
    write_fourcc("LIST");
    write_u32(0);
    write_fourcc("movi");
    s_movi_start = w_tell();

    ESP_LOGI(TAG, "AVI opened: %s (%dx%d @ %d fps)", path, width, height, s_fps);
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
    if (written != jpeg_size) { ESP_LOGE(TAG, "SD write err"); return false; }
    if (jpeg_size & 1) { uint8_t pad = 0; w_write(&pad, 1); }

    AVIIndexEntry *idx = &s_index[s_index_count++];
    memcpy(idx->ckid, "00dc", 4);
    idx->dwFlags  = 0x10;             /* KEYFRAME (every MJPEG is) */
    idx->dwOffset = chunk_offset;
    idx->dwSize   = jpeg_size;

    if (jpeg_size > s_max_frame_size) s_max_frame_size = jpeg_size;
    s_frame_count++;

    if (++s_flush_counter >= FLUSH_INTERVAL) {
        flush_header();
        s_flush_counter = 0;
    }
    return true;
}

bool avi_writer_close(void)
{
    if (!s_file_open) return false;

    /* Finalize movi */
    uint32_t movi_end = w_tell();
    uint32_t movi_data_size = movi_end - s_movi_start + 4;
    w_seek(s_movi_start - 8);
    write_u32(movi_data_size);
    w_seek(movi_end);

    /* idx1 */
    write_fourcc("idx1");
    uint32_t idx_size = s_index_count * sizeof(AVIIndexEntry);
    write_u32(idx_size);
    if (s_index && s_index_count > 0) {
        w_write(s_index, idx_size);
    }

    /* RIFF size */
    uint32_t file_end = w_tell();
    w_seek(4);
    write_u32(file_end - 8);

    /* Patch avih + video strh with measured timing */
    uint32_t duration_ms = now_ms() - s_start_ms;
    uint64_t usec_per_frame = (s_frame_count > 0 && duration_ms > 0)
        ? ((uint64_t)duration_ms * 1000ULL) / s_frame_count
        : (uint64_t)(1000000 / s_fps);
    if (usec_per_frame == 0) usec_per_frame = 1000000 / s_fps;

    w_seek(32);
    MainAVIHeader avih = {0};
    avih.dwMicroSecPerFrame = (uint32_t)usec_per_frame;
    avih.dwMaxBytesPerSec   = (duration_ms > 0)
        ? (uint32_t)((uint64_t)(movi_end - s_movi_start) * 1000ULL / duration_ms)
        : (s_max_frame_size * s_fps);
    avih.dwFlags            = 0x0010;
    avih.dwTotalFrames      = s_frame_count;
    avih.dwStreams          = 1;
    avih.dwSuggestedBufferSize = s_max_frame_size;
    avih.dwWidth            = s_width;
    avih.dwHeight           = s_height;
    w_write(&avih, sizeof(avih));

    if (s_strh_vid_offset > 0) {
        w_seek(s_strh_vid_offset + 20);
        write_u32((uint32_t)usec_per_frame);   /* dwScale */
        write_u32(1000000);                    /* dwRate  */
        w_seek(s_strh_vid_offset + 32);
        write_u32(s_frame_count);              /* dwLength */
    }

    w_close_truncated(file_end);

    if (s_index) { heap_caps_free(s_index); s_index = NULL; }
    s_index_cap = 0;

    float fps_meas = (duration_ms > 0)
        ? (float)s_frame_count * 1000.0f / duration_ms : 0.0f;
    ESP_LOGI(TAG, "AVI closed: %lu frames, %lu B, %.2f fps",
             (unsigned long)s_frame_count, (unsigned long)file_end, fps_meas);

    uint32_t saved = s_frame_count;
    s_frame_count = 0;
    s_index_count = 0;
    s_max_frame_size = 0;
    return saved > 0;
}

bool     avi_writer_is_open(void)     { return s_file_open; }
uint32_t avi_writer_frame_count(void) { return s_frame_count; }
uint32_t avi_writer_file_size(void)   { return s_file_open ? (uint32_t)f_size(&s_fil) : 0; }
