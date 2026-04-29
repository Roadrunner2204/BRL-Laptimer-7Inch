"""
DashConfig schema — mirrors main/ui/dash_config.h on the laptimer.

Field IDs and zone sizes are kept in sync by hand. If the firmware ever
adds a new FIELD_*, mirror it here and add a translation in FIELD_LABELS.

Wire format (JSON, exchanged via /dash_config — endpoint to be added on
the firmware side; until then "Lokal speichern" + manual SD copy works):

    {
        "language": 0,
        "units": 0,
        "delta_scale_ms": 1000,
        "z1": [2, 4, 1, 5, 3],
        "z2": [6, 7, 8],
        "z3": [32, 33, 34, 35, 36],
        "veh_conn_mode": 0,
        "show_obd": 1
    }
"""

from __future__ import annotations

# Field IDs from main/ui/dash_config.h
FIELD_NONE      = 0
FIELD_SPEED     = 1
FIELD_LAPTIME   = 2
FIELD_BESTLAP   = 3
FIELD_DELTA_NUM = 4
FIELD_LAP_NR    = 5
FIELD_SECTOR1   = 6
FIELD_SECTOR2   = 7
FIELD_SECTOR3   = 8
FIELD_MAP       = 9
FIELD_RPM       = 32
FIELD_THROTTLE  = 33
FIELD_BOOST     = 34
FIELD_COOLANT   = 35
FIELD_INTAKE    = 36
FIELD_LAMBDA    = 37
FIELD_BRAKE     = 38
FIELD_STEERING  = 39
FIELD_BATTERY   = 40
FIELD_MAF       = 41
FIELD_AN1       = 64
FIELD_AN2       = 65
FIELD_AN3       = 66
FIELD_AN4       = 67


FIELD_LABELS: dict[int, str] = {
    FIELD_NONE:      "— leer —",
    FIELD_SPEED:     "Speed",
    FIELD_LAPTIME:   "Laptime",
    FIELD_BESTLAP:   "Best Lap",
    FIELD_DELTA_NUM: "Δ Live",
    FIELD_LAP_NR:    "Lap-Nr.",
    FIELD_SECTOR1:   "Sektor 1",
    FIELD_SECTOR2:   "Sektor 2",
    FIELD_SECTOR3:   "Sektor 3",
    FIELD_MAP:       "Karte",
    FIELD_RPM:       "RPM",
    FIELD_THROTTLE:  "Throttle %",
    FIELD_BOOST:     "Boost / MAP",
    FIELD_COOLANT:   "Coolant °C",
    FIELD_INTAKE:    "Intake °C",
    FIELD_LAMBDA:    "Lambda",
    FIELD_BRAKE:     "Brake %",
    FIELD_STEERING:  "Lenkwinkel",
    FIELD_BATTERY:   "Batterie V",
    FIELD_MAF:       "MAF g/s",
    FIELD_AN1:       "AN1",
    FIELD_AN2:       "AN2",
    FIELD_AN3:       "AN3",
    FIELD_AN4:       "AN4",
}

Z1_FIELDS = [FIELD_NONE, FIELD_SPEED, FIELD_LAPTIME, FIELD_BESTLAP,
             FIELD_DELTA_NUM, FIELD_LAP_NR, FIELD_SECTOR1, FIELD_SECTOR2,
             FIELD_SECTOR3, FIELD_MAP]
Z2_FIELDS = [FIELD_NONE, FIELD_SECTOR1, FIELD_SECTOR2, FIELD_SECTOR3,
             FIELD_DELTA_NUM, FIELD_BESTLAP, FIELD_LAPTIME]
Z3_FIELDS = [FIELD_NONE, FIELD_RPM, FIELD_THROTTLE, FIELD_BOOST,
             FIELD_COOLANT, FIELD_INTAKE, FIELD_LAMBDA, FIELD_BRAKE,
             FIELD_STEERING, FIELD_BATTERY, FIELD_MAF,
             FIELD_AN1, FIELD_AN2, FIELD_AN3, FIELD_AN4]

Z1_SLOTS = 5
Z2_SLOTS = 3
Z3_SLOTS = 5

DELTA_SCALE_OPTIONS_MS = [2000, 3000, 5000, 10000, 20000]
LANGUAGE_LABELS = {0: "Deutsch", 1: "English"}
UNITS_LABELS = {0: "Metrisch (km/h)", 1: "Imperial (mph)"}
VEH_CONN_LABELS = {0: "OBD BLE", 1: "CAN direct"}


def default_dash_config() -> dict:
    return {
        "language": 0,
        "units": 0,
        "delta_scale_ms": 1000,
        "z1": [FIELD_LAPTIME, FIELD_BESTLAP, FIELD_DELTA_NUM,
               FIELD_LAP_NR, FIELD_SPEED],
        "z2": [FIELD_SECTOR1, FIELD_SECTOR2, FIELD_SECTOR3],
        "z3": [FIELD_RPM, FIELD_THROTTLE, FIELD_BOOST,
               FIELD_COOLANT, FIELD_BRAKE],
        "veh_conn_mode": 0,
        "show_obd": 1,
    }
