/**
 * lv_code.cpp — LVGL UI entry point
 *
 * Called once from main.c after LVGL is fully initialised.
 * Delegates to app_init() in ui/app.cpp.
 */

#include "lvgl.h"
#include "ui/app.h"

extern "C" void lv_my_setup(void) {
    app_init();
}
