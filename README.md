# BRL Laptimer v2.0 — ESP32-P4

GPS lap timer for motorsport on the **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B**.

## Hardware

| Component        | Specification                                    |
|------------------|--------------------------------------------------|
| MCU              | ESP32-P4, dual-core RISC-V @ 360 MHz             |
| Memory           | 32 MB PSRAM (hex-mode, 200 MHz)                  |
| Flash            | 16 MB QIO                                        |
| Display          | 7" 1024x600 MIPI DSI (EK79007 driver)            |
| Touch            | GT911 capacitive, 5-point, I2C                   |
| Wi-Fi / BT       | ESP32-C6 co-processor (Wi-Fi 6 / BLE 5) via SDIO |
| GPS              | u-blox TAU1201, UART + PPS                       |
| CAN Bus          | SN65HVD230 transceiver module                    |
| SD Card          | SDMMC 4-bit, FAT32                               |

## Pin Assignments

### GPS Module (u-blox TAU1201)

| Signal | GPIO | Header Pin | Notes                    |
|--------|------|------------|--------------------------|
| RX     | 2    | IO2        | ESP32 RX <- TAU1201 TX   |
| TX     | 3    | IO3        | ESP32 TX -> TAU1201 RX   |
| PPS    | 4    | IO4        | Pulse-per-second input   |

### CAN Bus (SN65HVD230 Transceiver)

| Signal | GPIO | Header Pin | Notes                         |
|--------|------|------------|-------------------------------|
| TX     | 5    | IO5        | ESP32 TX -> SN65HVD230 D pin  |
| RX     | 28   | IO28       | SN65HVD230 R pin -> ESP32     |

SN65HVD230 wiring:
- **VCC** -> 3.3V
- **GND** -> GND
- **D** (Driver Input) -> GPIO 5
- **R** (Receiver Output) -> GPIO 28
- **CANH / CANL** -> Vehicle CAN bus (e.g. OBD-II pin 6/14)

### ESP32-C6 Co-Processor (esp_hosted, SDIO Slot 1)

| Signal      | GPIO | Notes                     |
|-------------|------|---------------------------|
| CLK         | 18   | SDIO clock                |
| CMD         | 19   | SDIO command              |
| D0          | 14   | SDIO data 0              |
| D1          | 15   | SDIO data 1              |
| D2          | 16   | SDIO data 2              |
| D3          | 17   | SDIO data 3              |
| RESET       | 54   | C6 slave reset            |

**Note:** GPIO 14-19 are reserved for the C6 SDIO interface. Do not use these for other peripherals.

### SD Card (SDMMC, managed by BSP)

The SD card uses the board's built-in SDMMC slot (directly on the Waveshare board, active-low power via GPIO 42).

### Display

| Signal    | GPIO | Notes                       |
|-----------|------|-----------------------------|
| Backlight | 33   | PWM via LEDC (GPIO 32 reserved by board) |

Display uses MIPI DSI (directly managed by BSP, no user-facing GPIOs).

### Free Header Pins

| GPIO | Header Pin | Status    |
|------|------------|-----------|
| 29   | IO29       | Available |
| 30   | IO30       | Available |
| 31   | IO31       | Available |
| 34   | IO34       | Available |
| 36   | IO36       | Available |

## Architecture

```
Core 0 — Logic Task         Core 1 — LVGL Task (BSP)
  GPS parsing                 UI rendering
  Lap timing                  Touch input
  OBD-II BLE (NimBLE)         Screen management
  CAN bus (TWAI)
  WiFi manager
  Session storage
```

## Vehicle Data Connection

Two modes selectable in Settings:

| Mode            | Hardware              | Data Source                    |
|-----------------|-----------------------|--------------------------------|
| **OBD Dongle**  | BRL BLE OBD Adapter   | Standard OBD-II PIDs via BLE   |
| **CAN Bus**     | SN65HVD230 module     | Direct CAN signals via .brl profile |

- **OBD Dongle:** Queries 5 standard PIDs (RPM, throttle, MAP, coolant, intake) at ~5 Hz via BLE
- **CAN Bus:** Passive listener (listen-only mode), decodes all signals defined in the active car profile at full bus speed. Requires a `.brl` car profile to be loaded.

## Software Stack

- **Framework:** ESP-IDF v5.4.1
- **UI:** LVGL 9.2
- **BSP:** `waveshare/esp32_p4_wifi6_touch_lcd_7b`
- **WiFi:** `esp_hosted` + `esp_wifi_remote` (transparent proxy to C6)
- **Bluetooth:** NimBLE host on P4, controller on C6 via VHCI
- **CAN:** ESP32-P4 TWAI peripheral + SN65HVD230 transceiver

## Project Structure

```
main/
  main.c                App entry (app_main), task setup
  lv_code.cpp           LVGL bridge (BSP init -> app.cpp)
  data/
    lap_data.h          Global state, track/session structs
    track_db.h          Built-in European track database (11 tracks)
    car_profile.cpp/h   Encrypted .brl profile parser, server download
  gps/
    gps.cpp/h           u-blox TAU1201 NMEA parser, PPS handling
  timing/
    lap_timer.cpp/h     GPS-based lap/sector timing
    live_delta.cpp/h    Real-time delta calculation
  can/
    can_bus.cpp/h       Direct CAN bus via TWAI + SN65HVD230
  obd/
    obd_bt.cpp/h        OBD-II via BLE (NimBLE, BRL OBD Adapter)
  wifi/
    wifi_mgr.cpp/h      AP mode (data server) + STA mode (internet)
    data_server.cpp/h   HTTP server for session download
  storage/
    sd_mgr.cpp/h        SD card mount + file helpers
    session_store.cpp/h Session/track persistence (JSON on SD)
  ui/
    app.cpp             All UI screens (menu, timing, settings, etc.)
    screen_timing.cpp   Timing dashboard with configurable zones
    dash_config.cpp/h   Dashboard layout config (5 data zones)
    i18n.cpp/h          Translations (DE/EN)
    brl_fonts.h         Custom Montserrat fonts (14-160pt)
    fonts/              Font source files
sdkconfig.defaults      SDK configuration
partitions.csv          Flash partition table (14MB app, 2MB storage)
```

## Build

```bash
# ESP-IDF Terminal (VS Code ESP-IDF extension recommended)
idf.py build
idf.py -p COMx flash monitor
```

## WiFi Modes

- **AP Mode:** Creates "BRL-Laptimer" hotspot (192.168.4.1). Clients connect to download session data via HTTP.
- **STA Mode:** Connects to external WiFi for car profile downloads from `downloads.bavarian-racelabs.com`.

## Car Profiles

Encrypted `.brl` files containing CAN bus sensor definitions per vehicle. Profiles are downloaded over HTTPS (STA mode) and stored on SD card (`/sdcard/cars/`).

Server-side `list.txt` format:
```
N47F.brl;BMW;N47 F-Series
S65E.brl;BMW;S65 E-Series
i30N.brl;Hyundai;2.0 GDI
```
