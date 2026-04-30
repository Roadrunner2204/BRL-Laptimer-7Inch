# BRL OBD Adapter — Firmware

ESP32-S3 Super Mini (4 MB Flash, kein PSRAM) mit SN65HVD230 CAN-Transceiver
und L9637D K-Line-Treiber. Reicht BLE → CAN/K-Line durch.

## Build & Flash

PlatformIO (über VS Code-Extension oder CLI):

```bash
cd adapter-firmware
pio run                  # Build only
pio run -t upload        # Build + Flash über USB
pio device monitor       # Serial-Log @ 115200
```

Board: `adafruit_feather_esp32s3_nopsram` (geht für das Super-Mini-Pinout
mit 4 MB Flash override in `platformio.ini`).

## BLE Wire-Protocol

- Service-UUID: `0000FFE0-...` (Custom Binary)
- CMD-Charakteristik: `0000FFE1-...` Write
- RESP-Charakteristik: `0000FFE2-...` Notify
- Plus NUS-Service `6E400001-...` für ELM327-Emulation (Torque Pro etc.)

Frame-Format: `[CMD] [Payload...]` rein, `[CMD] [STATUS] [Payload...]` raus.

Wichtige Commands (siehe `include/ble_server.h` für vollständige Liste):

| Code   | Name              | Payload                  | Bemerkung |
|--------|-------------------|--------------------------|-----------|
| `0x05` | `READ_MULTI_PID`  | `[PID1..PID6]`           | Mode-01 multi-PID @ 0x7E0/0x7E8 |
| `0x10` | `READ_DID_22`     | `[DID_hi][DID_lo]`       | Standard UDS @ 0x7E0/0x7E8 (B-/N-Series Benziner) |
| `0x16` | `READ_DID_BMW`    | `[TARGET][DID_hi][DID_lo]` | **BMW Extended-Addressing** — F-Series + späte E-Series N47/N57 DDE |
| `0x21` | `SET_ECU_DIAG`    | `[]`                     | Schaltet `READ_DID_22` auf 0x6F1/0x612 (Legacy, nicht von Laptimer genutzt) |

### `READ_DID_BMW` (0x16) — der Hauptfix für N47-DDE-Stille

**Wire vor BLE:**
```
TX → Adapter:  16 12 52 0F           (target 0x12, DID 0x520F)
```

**Wire vor CAN (Adapter setzt das so um):**
```
ID 0x6F1, Daten: 12 03 22 52 0F 00 00 00
                  │   │  │   └──┴── DID
                  │   │  └─ UDS ReadDataByIdentifier
                  │   └─ ISO-TP Single-Frame, length=3
                  └── Target = DDE/DME (0x12)
```

**ECU antwortet auf `0x612`:**
```
ID 0x612, Daten: F1 06 62 52 0F XX YY 00
                  │   │  │   └──┴── data...
                  │   │  └─ Response-SID
                  │   └─ ISO-TP SF, length=6
                  └── Source = Tester (immer 0xF1)
```

**Adapter → BLE-Client:**
```
RX ← Adapter: 16 00 62 52 0F XX YY    (CMD 0x16, STATUS OK, SVC 0x62, DID, data)
```

Target-Bytes (BMW Diag-Adressen):
- `0x12` — DDE/DME (Engine), DDE6.x/DDE7.x Diesel + N47/N54/N55/B47/B58
- `0x18` — EGS (Automatik-Getriebe)
- `0x29` — DSC (ABS/ESP)
- `0x40` — ZGW (Zentral-Gateway)

## Architektur

- `main.cpp` — BLE-Command-Dispatch, Bus-Init
- `ble_server.{h,cpp}` — NimBLE GATT-Server + NUS für ELM-Emulation
- `isotp.{h,cpp}` — ISO-TP (15765-2) mit BMW-Extended-Addressing (`setBmwExtended`)
- `uds_client.{h,cpp}` — UDS-Services 0x10/0x14/0x19/0x22/0x2C/0x3E
- `kline.{h,cpp}` — KWP2000 K-Line (für ältere E36/E46/MS41)
- `elm_emulator.{h,cpp}` — ELM327/STN-AT-Command-Parser auf NUS

## Auto-CAN-Init beim Boot

Setup() macht `canInit(CAN_BAUD_DCAN)` automatisch — D-CAN/PT-CAN @ 500 kBit/s
ist der Bus, auf dem 99 % der BMW-Diagnose-Daten liegen (ab E60/E9x LCI bis
heute). Wer K-Line oder K-CAN braucht, schickt explizit `SET_BUS_KLINE` /
`SET_BUS_CAN [1]`.

## History

- **2026-04-30** — `READ_DID_BMW` (0x16) + ISO-TP Extended-Addressing eingebaut.
  Vor diesem Fix schwiegen alle Mode-22-DIDs, weil der Adapter sie auf
  Standard-OBD2-Adressen (0x7E0/0x7E8) routete — BMW DDE hört da nur
  Mode-01-PIDs ab. Plus Auto-CAN-Init im Boot-Path.
