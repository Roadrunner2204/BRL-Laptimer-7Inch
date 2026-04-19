#pragma once
#include <stdbool.h>
#include <stddef.h>

/**
 * sd_mgr -- SD card manager
 *
 * On the Waveshare ESP32-P4 board the SD card is managed by the BSP.
 * This header declares the interface used by other modules.
 */

#ifdef __cplusplus
extern "C" {
#endif

bool  sd_mgr_init(void);
bool  sd_mgr_available(void);

// Diagnostic: write 10 MB via f_write to measure real sustained throughput.
// Called once after mount. Creates and deletes /sdcard/_benchmark.bin.
// Result logged as "SD benchmark: X.XX MB/s (N chunks of K KB)".
void  sd_mgr_benchmark(void);

bool  sd_write_file(const char *path, const char *data, size_t len);
bool  sd_append_file(const char *path, const char *data, size_t len);
bool  sd_read_file(const char *path, char *buf, size_t buf_size);
bool  sd_file_exists(const char *path);
bool  sd_delete_file(const char *path);
bool  sd_make_dir(const char *path);

#ifdef __cplusplus
}
#endif
