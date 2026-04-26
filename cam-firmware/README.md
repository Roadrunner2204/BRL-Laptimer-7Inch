# BRL Laptimer Camera Module Firmware

Companion firmware to the main `BRL-Laptimer` repo. Runs on a separate
**DFRobot FireBeetle 2 ESP32-P4 (DFR1172)** with a **Raspberry Pi Camera
v1.3 (OV5647)** on the MIPI-CSI port. Records HD video to its own microSD
card and mirrors the laptimer's telemetry stream over a 4-pin UART cable
so the offline workflow works without touching the laptimer.

## Why a separate firmware

The single-chip design hit physical limits — see commit `e08cb92` in the
main repo: USB-ISO ↔ SDMMC DMA contention deckelte SD-Schreibrate auf
500 KB/s and JPEG+LVGL+WiFi6 sättigten beide Cores. Splitting the camera
onto its own ESP32-P4 frees the main board for timing/UI/WiFi and gives
the cam exclusive use of its SDMMC + JPEG pipeline.

## Hardware

| Part | Notes |
|---|---|
| DFR1172 FireBeetle 2 ESP32-P4 | 32 MB PSRAM, 16 MB Flash, ESP32-C6 onboard, MIPI-CSI |
| RPi Camera v1.3 (OV5647)       | 1080p30 RAW10 — only RPi cam with mature ESP-IDF driver |
| 15-pin 1mm FPC cable           | Standard RPi length (~10 cm) |
| microSD ≥ 64 GB U3/V30         | Onboard SDIO slot |
| 4-pin M8 cable to laptimer     | 5 V + GND + UART TX/RX |

UART2 pinout (cam side, free header pins):

| Signal | DFR1172 GPIO | Goes to laptimer GPIO |
|--------|--------------|-----------------------|
| TX     | 17           | 31 (laptimer RX)      |
| RX     | 18           | 30 (laptimer TX)      |

## Wire protocol

`main/cam_link/cam_link_protocol.h` is a **byte-for-byte copy** of the
laptimer's `../../main/camera_link/cam_link_protocol.h`. Both files MUST
stay in sync; CI should diff them. Frame format:

```
[SOF=0xA5] [TYPE u8] [LEN u16-LE] [PAYLOAD ... LEN bytes] [CRC8/MAXIM]
```

See the header for frame types, payload structs, and direction.

## Build & flash

```bash
cd cam-firmware
idf.py set-target esp32p4   # one-time
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

This is a **separate ESP-IDF project** from the main laptimer in
`../main/`. Switch IDF build directories before flashing.

## Bring-up status

Skeleton only — frame transport works, capture / WiFi / HTTP are stubs:

| Module | Status |
|---|---|
| `cam_link/cam_link_uart` | done — UART2 RX framer + TX sender + STATUS heartbeat |
| `recorder/recorder`      | stub — TODO: esp_video + JPEG encoder + AVI writer (recycle from `e08cb92`) + telemetry sidecar in `.brl` format |
| `wifi_sta/wifi_sta`      | stub — TODO: esp_hosted bring-up, join `BRL-Laptimer` AP |
| `http_server/http_server`| stub — TODO: `GET /videos/list`, `GET /video/<id>` |

Next implementation milestones in order:

1. **MIPI-CSI bring-up** with `esp_video` on the OV5647 — adapt
   `examples/peripherals/isp/multi_pipelines`.
2. **AVI writer** — recycle `main/video/avi_writer.{h,cpp}` from main
   repo commit `e08cb92` (still in git history).
3. **Telemetry sidecar** — port the laptimer's `session_store` writer to
   write `.brl` to `/sdcard/sessions/<id>/telemetry.brl`.
4. **WiFi STA + HTTP server** — pattern from main's `wifi_mgr` and
   `data_server`.

## Layout

```
cam-firmware/
├── CMakeLists.txt          ESP-IDF project root
├── partitions.csv          16 MB flash, 3 OTA slots + storage
├── sdkconfig.defaults      ESP32-P4 / 32 MB PSRAM defaults
├── README.md               this file
└── main/
    ├── CMakeLists.txt      component manifest
    ├── main.c              app_main + frame dispatcher
    ├── cam_link/
    │   ├── cam_link_protocol.h   COPY OF MAIN — keep in sync!
    │   ├── cam_link_uart.h
    │   └── cam_link_uart.c
    ├── recorder/           STUB — see TODOs
    ├── wifi_sta/           STUB
    └── http_server/        STUB
```
