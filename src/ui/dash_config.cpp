#include "dash_config.h"
#include <Preferences.h>

DashConfig g_dash_cfg = {
    /* language */ 0,
    /* units    */ 0,
    /* z1 */ { FIELD_SPEED, FIELD_LAPTIME, FIELD_BESTLAP, FIELD_DELTA_NUM, FIELD_LAP_NR },
    /* z2 */ { FIELD_SECTOR1, FIELD_SECTOR2, FIELD_SECTOR3 },
    /* z3 */ { FIELD_RPM, FIELD_THROTTLE, FIELD_BOOST, FIELD_COOLANT, FIELD_NONE },
};

static Preferences s_prefs;

void dash_config_load() {
    s_prefs.begin("dashcfg", true);
    g_dash_cfg.language = s_prefs.getUChar("lang",  0);
    g_dash_cfg.units    = s_prefs.getUChar("units", 0);
    s_prefs.getBytes("z1", g_dash_cfg.z1, Z1_SLOTS);
    s_prefs.getBytes("z2", g_dash_cfg.z2, Z2_SLOTS);
    s_prefs.getBytes("z3", g_dash_cfg.z3, Z3_SLOTS);
    s_prefs.end();
}

void dash_config_save() {
    s_prefs.begin("dashcfg", false);
    s_prefs.putUChar("lang",  g_dash_cfg.language);
    s_prefs.putUChar("units", g_dash_cfg.units);
    s_prefs.putBytes("z1", g_dash_cfg.z1, Z1_SLOTS);
    s_prefs.putBytes("z2", g_dash_cfg.z2, Z2_SLOTS);
    s_prefs.putBytes("z3", g_dash_cfg.z3, Z3_SLOTS);
    s_prefs.end();
}
