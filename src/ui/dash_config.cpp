#include "dash_config.h"
#include <Preferences.h>

DashConfig g_dash_cfg = { WDGT_DEFAULT_MASK };

static Preferences s_prefs;

void dash_config_load() {
    s_prefs.begin("dashcfg", true);
    g_dash_cfg.visible_mask = s_prefs.getUInt("mask", WDGT_DEFAULT_MASK);
    s_prefs.end();
}

void dash_config_save() {
    s_prefs.begin("dashcfg", false);
    s_prefs.putUInt("mask", g_dash_cfg.visible_mask);
    s_prefs.end();
}
