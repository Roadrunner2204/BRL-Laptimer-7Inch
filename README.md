# BRL-Laptimer-7Inch

Lap timer project on the **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B**.

## Hardware

| Component        | Specification                                    |
|------------------|--------------------------------------------------|
| MCU              | ESP32-P4, dual-core RISC-V @ 360 MHz             |
| Memory           | 32 MB PSRAM (hex-mode, 200 MHz)                  |
| Flash            | 16 MB QIO                                        |
| Display          | 7" 1024x600 MIPI DSI (EK79007 driver)            |
| Touch            | GT911 capacitive, 5-point, I2C                   |
| Wi-Fi / BT       | ESP32-C6 co-processor (Wi-Fi 6 / BLE 5)          |
| Audio            | ES8311 codec + NS4150B amplifier                  |
| Camera           | MIPI-CSI 2-lane (optional OV5647)                |
| USB              | USB 2.0 OTG HS                                   |

## Software Stack

- **Framework:** ESP-IDF v5.4+
- **UI Library:** LVGL 9.2
- **BSP:** `waveshare/esp32_p4_wifi6_touch_lcd_7b` (ESP Component Registry)

## Build

```bash
# Set up ESP-IDF environment first, then:
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Project Structure

```
CMakeLists.txt          Top-level ESP-IDF build file
sdkconfig.defaults      Default SDK configuration
partitions.csv          Flash partition table
main/
  CMakeLists.txt        Main component build file
  idf_component.yml     Component dependencies (BSP, LVGL)
  main.c                Display/touch init via Waveshare BSP
  lv_code.c             UI layer (lap timer widgets)
```