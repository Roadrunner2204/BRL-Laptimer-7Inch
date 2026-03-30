/**
 * lv_code.cpp
 * LVGL UI elements and callbacks for BRL Laptimer
 *
 * Add your lap timer UI here.
 * This file is intentionally kept separate from main.cpp so the
 * display/touch initialisation and the application UI can evolve
 * independently.
 */

#include <Arduino.h>
#include <lvgl.h>

// ---------------------------------------------------------------------------
// UI objects
// ---------------------------------------------------------------------------
static lv_obj_t *label1  = nullptr;
static lv_obj_t *slider1 = nullptr;
static lv_obj_t *btn1    = nullptr;

// ---------------------------------------------------------------------------
// Event callbacks
// ---------------------------------------------------------------------------
static void btn_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED) {
    static uint8_t cnt = 0;
    cnt++;
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    lv_label_set_text_fmt(label, "Button: %d", cnt);
  }
}

static void slider_event_cb(lv_event_t *e)
{
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  lv_label_set_text_fmt(label1, "%" LV_PRId32, lv_slider_get_value(slider));
  lv_obj_align_to(label1, slider, LV_ALIGN_OUT_TOP_MID, 0, -15);
}

// ---------------------------------------------------------------------------
// Main UI setup — called once from main.cpp setup()
// ---------------------------------------------------------------------------
void lv_my_setup()
{
  // --- Slider ---
  slider1 = lv_slider_create(lv_screen_active());
  lv_slider_set_value(slider1, 0, LV_ANIM_ON);
  lv_obj_set_pos(slider1, 40, 60);
  lv_obj_set_size(slider1, 200, 20);
  lv_obj_add_event_cb(slider1, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *slider_label = lv_label_create(slider1);
  lv_label_set_text(slider_label, "Slider");
  lv_obj_center(slider_label);

  // --- Value label (above slider) ---
  label1 = lv_label_create(lv_screen_active());
  lv_label_set_text(label1, "0");
  lv_obj_align_to(label1, slider1, LV_ALIGN_OUT_TOP_MID, 0, -15);

  // --- Button ---
  btn1 = lv_button_create(lv_screen_active());
  lv_obj_set_pos(btn1, 40, 120);
  lv_obj_set_size(btn1, 100, 50);
  lv_obj_add_event_cb(btn1, btn_event_cb, LV_EVENT_ALL, NULL);

  lv_obj_t *btn_label = lv_label_create(btn1);
  lv_label_set_text(btn_label, "Button");
  lv_obj_center(btn_label);
}
