#pragma once
/**
 * compat.h — Arduino API compatibility shim for ESP-IDF
 *
 * Provides millis(), delay(), Serial-like logging macros so that
 * the application logic can be ported with minimal changes.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* millis() — returns ms since boot */
static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* delay(ms) — FreeRTOS-based delay */
static inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ps_malloc — PSRAM allocation */
static inline void *ps_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

#ifdef __cplusplus
}
#endif

/* Serial.printf / Serial.println replacements — use ESP_LOG macros.
 * Each file should define:  static const char *TAG = "module_name";
 * Then use: log_i(...), log_e(...), log_w(...)                     */
#define log_e(fmt, ...)  ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define log_i(fmt, ...)  ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define log_w(fmt, ...)  ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
