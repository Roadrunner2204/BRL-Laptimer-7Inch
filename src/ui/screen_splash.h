#pragma once
#include <lvgl.h>

/**
 * Splash-Screen beim Booten.
 *
 * splash_show() zeigt den Startscreen.
 * Nach 'duration_ms' wird on_done() aufgerufen.
 *
 * Sobald du dein Logo-JPEG hast:
 *   1. logo_placeholder.h durch echte logo_data.h ersetzen
 *   2. In screen_splash.cpp: LOGO_USE_PLACEHOLDER entfernen und
 *      lv_image_set_src(img, &logo_dsc) aktivieren.
 */
void splash_show(uint32_t duration_ms, void (*on_done)());
