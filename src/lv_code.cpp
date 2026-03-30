/**
 * lv_code.cpp — LVGL UI entry point
 *
 * Called once from main.cpp after LVGL is fully initialised.
 * Delegates to app_init() in ui/app.cpp.
 */

#include <Arduino.h>
#include <lvgl.h>
#include "ui/app.h"

void lv_my_setup() {
    app_init();
}
