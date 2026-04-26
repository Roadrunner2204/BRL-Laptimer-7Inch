#pragma once
#include "lvgl.h"

// ---------------------------------------------------------------------------
// BRL Laptimer — main application UI
// Call app_init() once from lv_my_setup().
// Call app_tick() from loop() or a periodic timer to update live values.
// ---------------------------------------------------------------------------

void app_init();
void app_tick();   // updates live data on the dashboard (call ~100 ms)

// ---------------------------------------------------------------------------
// Sub-screen helpers — exposed so individual screen modules (e.g.
// screen_video_settings.cpp) can build sub-screens with the same
// status-bar / header / content-area skeleton everything else uses.
// ---------------------------------------------------------------------------
lv_obj_t *make_sub_screen(const char *title, lv_event_cb_t back_cb,
                          lv_obj_t **action_btn_out = nullptr,
                          const char *action_label  = nullptr);
lv_obj_t *build_content_area(lv_obj_t *scr, bool scrollable = true);
void      sub_screen_load(lv_obj_t *scr);
void      open_settings_screen(void);
