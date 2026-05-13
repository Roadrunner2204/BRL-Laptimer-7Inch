#!/usr/bin/env python3
"""
Universal-OBD.brl Generator
============================
Erzeugt eine .brl-Datei mit ALLEN gängigen SAE J1979 / ISO 15031 Mode-01 PIDs.
Das ist die "alles was Torque Pro auch kann"-Sensorliste — herstellerunabhängig.

Das Display lädt diese .brl wenn der Benutzer "OBD-Modus" wählt. Welche PIDs
das konkrete Auto wirklich unterstützt, kommt vom CMD_DISCOVER_PIDS-Aufruf
beim BLE-Connect (Adapter-Firmware ≥ v1.1) — Display filtert dann die
unterstützten aus dieser Liste raus.

Aufruf:
  python3 tools/gen_universal_obd_brl.py
  → schreibt "Can Data/OBD.brl"

Format pro Sensor (entspricht TRI-Format mit 26 Feldern als Strings):
  proto="7DF"           OBD2 broadcast
  can_id="2XXYY"        "2" prefix + "01" mode + "YY" PID (low byte = PID)
  fmt="0"               big-endian
  start="3"             erste Daten-Byte = data[3] (nach Header [0x41][PID])
  len, unsigned, scale, offset etc. wie SAE J1979 vorschreibt
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from brl_format import create_brl


def make_pid(pid: int, name: str, length: int, scale: float, offset: float,
             unit_type: int = 0, unsigned: int = 1,
             vmin: float = -1, vmax: float = 100, fmt: int = 0,
             start: int = 3) -> dict:
    """Standard-Mode-01 PID-Sensor.
    unit_type: 0=keine, 1=Druck, 2=Temp, 3=Geschw, 4=Lambda
    """
    return {
        "proto":      "7DF",
        "can_id":     f"201{pid:02X}",
        "fmt":        str(fmt),
        "start":      str(start),
        "len":        str(length),
        "unsigned":   str(unsigned),
        "shift":      "0",
        "mask":       "0",
        "decimals":   "0",
        "name":       name,
        "scale":      str(scale),
        "offset":     str(offset),
        "mapper_type": "0",
        "mapper1":    "0",
        "mapper2":    "0",
        "mapper3":    "0",
        "mapper4":    "1",
        "ain":        "0",
        "min":        str(vmin),
        "max":        str(vmax),
        "ref_sensor": "255",
        "ref_val":    "0",
        "unused1":    "0",
        "popup":      "0",
        "unused2":    "0",
        "type":       str(unit_type),
    }


# Komplette Sensor-Liste — alle SAE J1979 Mode-01 PIDs die in irgendeinem
# Auto sinnvoll sind. Display filtert per DISCOVER_PIDS-Bitmap was wirklich
# verfügbar ist.
SENSORS = [
    # Engine basics
    make_pid(0x04, "Load",          1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x05, "WaterT",        1, 1,        -40,     2, vmin=-40,  vmax=130),
    make_pid(0x06, "ShrtTrmFT1",    1, 0.78125,  -100,    0, vmin=-25,  vmax=25),
    make_pid(0x07, "LngTrmFT1",     1, 0.78125,  -100,    0, vmin=-25,  vmax=25),
    make_pid(0x08, "ShrtTrmFT2",    1, 0.78125,  -100,    0, vmin=-25,  vmax=25),
    make_pid(0x09, "LngTrmFT2",     1, 0.78125,  -100,    0, vmin=-25,  vmax=25),
    make_pid(0x0A, "FuelP",         1, 3,        0,       1, vmin=0,    vmax=765),
    # PID 0x0B = Manifold Absolute Pressure (kPa absolute) per SAE-Standard.
    # NICHT "Boost" — das ist die Berechnung MAP-Baro (siehe computed_pids).
    make_pid(0x0B, "MAP",           1, 1,        0,       1, vmin=0,    vmax=255),
    make_pid(0x0C, "RPM",           2, 0.25,     0,       0, vmin=0,    vmax=8000),
    make_pid(0x0D, "Speed",         1, 1,        0,       3, vmin=0,    vmax=300),
    make_pid(0x0E, "IgnAdv",        1, 0.5,      -64,     0, vmin=-64,  vmax=64),
    make_pid(0x0F, "IntakeT",       1, 1,        -40,     2, vmin=-40,  vmax=120),
    make_pid(0x10, "MAF",           2, 0.01,     0,       0, vmin=0,    vmax=655),
    make_pid(0x11, "Throttle",      1, 0.392157, 0,       0, vmin=0,    vmax=100),
    # Lambda Bank 1 / Bank 2 Sensoren (Spannung)
    make_pid(0x14, "O2_B1S1_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x15, "O2_B1S2_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x16, "O2_B1S3_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x17, "O2_B1S4_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x18, "O2_B2S1_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x19, "O2_B2S2_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x1A, "O2_B2S3_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x1B, "O2_B2S4_V",     1, 0.005,    0,       4, vmin=0,    vmax=1.275),
    make_pid(0x1F, "RunTime",       2, 1,        0,       0, vmin=0,    vmax=65535),
    # Distance / Fuel rail
    make_pid(0x21, "DistMIL",       2, 1,        0,       0, vmin=0,    vmax=65535),
    make_pid(0x22, "FuelRailP",     2, 0.079,    0,       1, vmin=0,    vmax=5177),
    make_pid(0x23, "FuelRailGP",    2, 10,       0,       1, vmin=0,    vmax=655350),
    # EGR / Evap / Tank
    make_pid(0x2C, "CmdEGR",        1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x2D, "EGRerr",        1, 0.78125,  -100,    0, vmin=-100, vmax=100),
    make_pid(0x2E, "CmdEvap",       1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x2F, "FuelLevel",     1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x30, "Warmups",       1, 1,        0,       0, vmin=0,    vmax=255),
    make_pid(0x31, "DistClr",       2, 1,        0,       0, vmin=0,    vmax=65535),
    make_pid(0x33, "Baro",          1, 1,        0,       1, vmin=0,    vmax=255),
    # Cat temp 4 sensors
    make_pid(0x3C, "CatT_B1S1",     2, 0.1,      -40,     2, vmin=-40,  vmax=900),
    make_pid(0x3D, "CatT_B2S1",     2, 0.1,      -40,     2, vmin=-40,  vmax=900),
    make_pid(0x3E, "CatT_B1S2",     2, 0.1,      -40,     2, vmin=-40,  vmax=900),
    make_pid(0x3F, "CatT_B2S2",     2, 0.1,      -40,     2, vmin=-40,  vmax=900),
    # Mode 41+ — Voltage, Loads, Throttles, Pedals
    make_pid(0x42, "BattVolt",      2, 0.001,    0,       1, vmin=0,    vmax=20),
    make_pid(0x43, "AbsLoad",       2, 0.392157, 0,       0, vmin=0,    vmax=300),
    make_pid(0x44, "LambdaCmd",     2, 0.0000305,0,       4, vmin=0.5,  vmax=1.5),
    make_pid(0x45, "RelThrottle",   1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x46, "AmbientT",      1, 1,        -40,     2, vmin=-40,  vmax=80),
    make_pid(0x47, "ThrottleB",     1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x48, "ThrottleC",     1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x49, "PedalD",        1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x4A, "PedalE",        1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x4B, "PedalF",        1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x4C, "CmdThrottle",   1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x4D, "TimeMIL",       2, 1,        0,       0, vmin=0,    vmax=65535),
    make_pid(0x4E, "TimeClr",       2, 1,        0,       0, vmin=0,    vmax=65535),
    # PID 0x51 = Fuel Type (1 byte enum). Display liest das beim Connect für
    # AFR-Auto-Konfiguration. Hier als Sensor weil supportet auf vielen Autos.
    make_pid(0x51, "FuelType",      1, 1,        0,       0, vmin=0,    vmax=23),
    make_pid(0x52, "Ethanol",       1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x5A, "RelPedal",      1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x5B, "HybBattLife",   1, 0.392157, 0,       0, vmin=0,    vmax=100),
    make_pid(0x5C, "OilT",          1, 1,        -40,     2, vmin=-40,  vmax=180),
    make_pid(0x5D, "InjTiming",     2, 0.0078125,-210,    0, vmin=-210, vmax=302),
    make_pid(0x5E, "FuelRate",      2, 0.05,     0,       0, vmin=0,    vmax=3277),
    # Mode 61+ — Torque
    make_pid(0x61, "DemandTrq",     1, 1,        -125,    0, vmin=-125, vmax=125),
    make_pid(0x62, "ActualTrq",     1, 1,        -125,    0, vmin=-125, vmax=125),
    make_pid(0x63, "RefTrq",        2, 1,        0,       0, vmin=0,    vmax=65535),
    # Mode 70+ — Charged engine sensors (Diesel/turbo)
    make_pid(0x73, "ExhaustP",      2, 0.01,     0,       1, vmin=0,    vmax=655),
    make_pid(0x74, "TurboRPM",      2, 10,       0,       0, vmin=0,    vmax=655350),
    make_pid(0x77, "ChargeAirT",    2, 0.1,      -40,     2, vmin=-40,  vmax=200),
    # Mode 80+ — Engine condition
    make_pid(0x84, "ManifoldT",     1, 1,        -40,     2, vmin=-40,  vmax=215),
    # Mode A0+ — Vehicle data
    make_pid(0xA2, "CylFuelRate",   2, 0.000488, 0,       0, vmin=0,    vmax=32),
    make_pid(0xA6, "Odometer",      4, 0.1,      0,       0, vmin=0,    vmax=429496729),
]


VEHICLE = {
    "make":      "Universal",
    "model":     "OBD2",
    "engine":    "Generic",
    "name":      "SAE J1979 / ISO 15031",
    "year_from": 2008,        # CAN-OBD ist seit 2008 EU/US Pflicht
    "year_to":   9999,
    "can":       "OBD-D-CAN",
    "bitrate":   500,
}


def write_c_header(out_path: Path):
    """Erzeugt einen C-Header mit der Sensor-Liste als const Array.
    Wird vom Display direkt eingebunden — kein SD-Karte-Load nötig."""

    def esc(s: str) -> str:
        return s.replace('\\', '\\\\').replace('"', '\\"')

    lines = [
        "// =============================================================================",
        "//  Universal-OBD-PID-Liste — AUTO-GENERIERT von tools/gen_universal_obd_brl.py",
        "//  NICHT VON HAND BEARBEITEN. Stattdessen Generator-Script editieren und",
        "//  neu ausführen. Dann compile.",
        "// =============================================================================",
        "#pragma once",
        "#include \"../data/car_profile.h\"",
        "",
        f"#define UNIVERSAL_OBD_SENSOR_COUNT  {len(SENSORS)}",
        "",
        "// SAE J1979 / ISO 15031 Mode-01 PID-Definitionen.",
        "// Immer im Display-Binary verfügbar — funktioniert auch ohne SD-Karte.",
        "static const CarSensor UNIVERSAL_OBD_SENSORS[UNIVERSAL_OBD_SENSOR_COUNT] = {",
    ]
    def fmt_float(v) -> str:
        """C-Float-Literal: '0.0f' nicht '0f'. '0.5f' nicht '0.5f' (geht beides)."""
        f = float(v)
        s = repr(f)            # '0.0', '0.392157', '-40.0' etc.
        if '.' not in s and 'e' not in s and 'E' not in s:
            s += '.0'
        return s + 'f'

    for idx, s in enumerate(SENSORS):
        # CarSensor: name[32], can_id, proto, start, len, is_unsigned,
        #            scale, offset, min_val, max_val, type, slot
        proto_int = 7   # BRL_PROTO_OBD2_MODE1 (string "7DF" → enum 7 in firmware)
        can_id_int = int(s['can_id'], 16)
        lines.append(
            f"    {{ \"{esc(s['name'])}\", "
            f"0x{can_id_int:08X}u, {proto_int}, {s['start']}, {s['len']}, "
            f"{s['unsigned']}, "
            f"{fmt_float(s['scale'])}, {fmt_float(s['offset'])}, "
            f"{fmt_float(s['min'])}, {fmt_float(s['max'])}, "
            f"{s['type']}, {idx} }},"
        )
    lines.append("};")
    lines.append("")
    out_path.write_text("\n".join(lines), encoding='utf-8')


def main():
    brl_path = Path(__file__).parent.parent / "Can Data" / "OBD.brl"
    hdr_path = Path(__file__).parent.parent / "main" / "obd" / "universal_obd_pids.h"

    print(f"Sensoren: {len(SENSORS)}")
    print(f"Erzeuge {brl_path}")
    data = create_brl(VEHICLE, SENSORS)
    brl_path.write_bytes(data)
    print(f"  .brl Größe: {len(data)} Bytes")

    print(f"Erzeuge {hdr_path}")
    write_c_header(hdr_path)
    print(f"  C-Header: {hdr_path.stat().st_size} Bytes")
    print("OK.")


if __name__ == "__main__":
    main()
