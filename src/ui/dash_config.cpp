#include "dash_config.h"
#include <Preferences.h>

DashConfig g_dash_cfg = { WDGT_DEFAULT_MASK, 0, 0 };

static Preferences s_prefs;

void dash_config_load() {
    s_prefs.begin("dashcfg", true);
    g_dash_cfg.visible_mask = s_prefs.getUInt("mask", WDGT_DEFAULT_MASK);
    g_dash_cfg.language     = s_prefs.getUChar("lang", 0);
    g_dash_cfg.units        = s_prefs.getUChar("units", 0);
    s_prefs.end();
}

void dash_config_save() {
    s_prefs.begin("dashcfg", false);
    s_prefs.putUInt("mask",  g_dash_cfg.visible_mask);
    s_prefs.putUChar("lang", g_dash_cfg.language);
    s_prefs.putUChar("units",g_dash_cfg.units);
    s_prefs.end();
}
