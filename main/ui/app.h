#pragma once

// ---------------------------------------------------------------------------
// BRL Laptimer — main application UI
// Call app_init() once from lv_my_setup().
// Call app_tick() from loop() or a periodic timer to update live values.
// ---------------------------------------------------------------------------

void app_init();
void app_tick();   // updates live data on the dashboard (call ~100 ms)
