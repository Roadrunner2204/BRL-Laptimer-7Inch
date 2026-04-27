# BRL Laptimer v2.0 — ESP32-P4

GPS lap timer for motorsport on the **Waveshare ESP32-P4-WIFI6-Touch-LCD-7B**,
with an optional external **camera daughter board** (DFRobot FireBeetle 2
ESP32-P4 + RPi Cam v1.3) for HD video recording. Cam is fully optional — the
laptimer detects its presence via a 500 ms STATUS heartbeat and only surfaces
the video UI when a cam is actually connected.

## Hardware

| Component        | Specification                                    |
|------------------|--------------------------------------------------|
| MCU              | ESP32-P4, dual-core RISC-V @ 360 MHz             |
| Memory           | 32 MB PSRAM (hex-mode, 200 MHz)                  |
| Flash            | 16 MB QIO                                        |
| Display          | 7" 1024×600 MIPI DSI (EK79007 driver)            |
| Touch            | GT911 capacitive, 5-point, I2C                   |
| Wi-Fi / BT       | ESP32-C6 co-processor (Wi-Fi 6 / BLE 5) via SDIO |
| GPS              | u-blox TAU1201, UART + PPS, 10 Hz, L1+L5         |
| CAN Bus          | SN65HVD230 transceiver module                    |
| SD Card (fixed)  | SDMMC 4-bit, FAT32 — non-user-removable          |
| Camera (optional)| External DFR1172 + RPi Cam v1.3 OV5647 over UART2 + WiFi |

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

### ESP32-C6 Co-Processor (esp_hosted, SDIO Slot 1)

| Signal | GPIO | Notes              |
|--------|------|--------------------|
| CLK    | 18   | SDIO clock         |
| CMD    | 19   | SDIO command       |
| D0..D3 | 14..17 | SDIO data        |
| RESET  | 54   | C6 slave reset     |

GPIO 14-19 are reserved for the C6 SDIO interface.

### Display

Backlight on GPIO 33 (PWM via LEDC). Display panel uses MIPI DSI (managed by BSP).

### Camera Link (UART2 → external DFR1172 cam module)

| Signal | GPIO | Header Pin | Notes                              |
|--------|------|------------|------------------------------------|
| TX     | 30   | IO30       | Main TX → cam RX (115200 8N1)      |
| RX     | 31   | IO31       | Main RX ← cam TX                   |

Cable to the camera enclosure: **4-pin M8** (5 V + GND + UART TX/RX).

### Free Header Pins

| GPIO | Header Pin | Status    |
|------|------------|-----------|
| 29   | IO29       | Available |
| 34   | IO34       | Available |
| 36   | IO36       | Available |

## Architecture

```
Core 0 — Logic Task              Core 1 — LVGL Task (BSP)
  GPS parsing                      UI rendering
  Lap timing                       Touch input
  OBD-II BLE (NimBLE)              Screen management
  CAN bus (TWAI)                   Cam preview canvas paint
  WiFi manager
  Session storage
  cam_link UART (telemetry → cam)
  cam_preview HTTP fetch + JPEG decode (on demand)
```

Video capture, JPEG encode, AVI write and MJPEG SD streaming **do not run
on the laptimer MCU** — they live on the optional cam daughter board so the
main board's display, WiFi, and timing stay headroom-positive.

## Vehicle Data Connection

Two modes selectable in Settings:

| Mode            | Hardware              | Data Source                    |
|-----------------|-----------------------|--------------------------------|
| **OBD Dongle**  | BRL BLE OBD Adapter   | Standard OBD-II PIDs via BLE   |
| **CAN Bus**     | SN65HVD230 module     | Direct CAN signals via .brl profile |

## Software Stack

- **Framework:** ESP-IDF v5.4.1
- **UI:** LVGL 9.2
- **BSP:** `waveshare/esp32_p4_wifi6_touch_lcd_7b`
- **WiFi:** `esp_hosted` 2.10.0 + `esp_wifi_remote` ~1.3.0 (proxied to C6)
- **Bluetooth:** NimBLE host on P4, controller on C6 via VHCI
- **CAN:** ESP32-P4 TWAI peripheral + SN65HVD230 transceiver
- **JPEG decode:** `driver/jpeg_decode.h` (HW engine, used only for cam preview)

## Project Structure

```
main/                            Laptimer firmware (this directory)
  main.c                         app_main, logic task
  data/                          AppState, lap/session/track/car structs
  gps/                           u-blox TAU1201 NMEA parser, PPS, 10 Hz config
  timing/                        Lap/sector timing, live delta
  can/                           Direct CAN via TWAI + SN65HVD230
  obd/                           OBD-II via BLE (NimBLE)
  sensors/                       Analog inputs (4× ADC1)
  wifi/                          AP/STA mgr + HTTP data_server
  storage/                       SD mount + session/track persistence
  camera_link/                   ↔ cam module:
    cam_link_protocol.h          shared wire format (mirror in cam-firmware)
    cam_link.{h,cpp}             UART2 driver, framing, status tracking
    cam_link_pump.cpp            g_state → telemetry frame bridge
  ui/
    app.{h,cpp}                  Menus, settings, navigation
    screen_timing.cpp            Timing dashboard with configurable zones
    screen_video_settings.cpp    Video Settings screen (preview + status)
    cam_preview.{h,cpp}          HTTP fetch + HW JPEG decode + canvas paint
    dash_config.cpp/h            Dashboard layout config
    i18n.cpp/h                   Translations (DE/EN)
    brl_fonts.h                  Custom Montserrat fonts (14-160pt)

cam-firmware/                    Separate ESP-IDF project for the DFR1172
                                 cam daughter board — see
                                 cam-firmware/README.md
```

## Build

```bash
# Laptimer firmware (this directory)
idf.py build
idf.py -p COMx flash monitor

# Cam-firmware (separate project)
cd cam-firmware
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The two firmwares live in the same repo (monorepo) but are independent
ESP-IDF projects — switch the build directory before flashing the cam.

## WiFi Modes

- **AP Mode:** Hosts "BRL-Laptimer" hotspot (192.168.4.1). Phone + cam
  daughter board both connect here. The cam's `/preview.jpg` and
  `/video/<id>` are reached directly via the cam's leased IP after a
  302 redirect from the laptimer's `data_server`.
- **STA Mode:** Connects to external WiFi for car profile downloads.

## Car Profiles

Encrypted `.brl` files containing CAN bus sensor definitions per vehicle.
Stored on SD at `/sdcard/cars/`, downloaded over HTTPS in STA mode from
`downloads.bavarian-racelabs.com`.

## Camera Module (optional)

External DFRobot FireBeetle 2 ESP32-P4 (DFR1172) + Raspberry Pi Camera v1.3
(OV5647) housed as an in-car action cam, connected via 4-pin M8 cable.
Detected automatically — UI surfaces only when the link is up.

| Path | Used for |
|---|---|
| **UART2** (115200 8N1, 4-pin M8) | REC start/stop + live telemetry stream + STATUS heartbeat |
| **WiFi** (cam joins laptimer AP as STA) | Bulk video/telemetry download to phone or Studio software |

### Three download paths (all parallel)

1. **Phone app via laptimer WiFi** — phone hits `GET /videos` on the
   laptimer (192.168.4.1), gets the index, follows a 302 to the cam's IP.
2. **Studio (Windows) via WiFi** — same redirect path, larger files.
3. **Cam SD into a laptop card reader** — works without the laptimer at
   all. The cam writes a complete telemetry mirror (`telemetry.ndjson`)
   alongside `video.avi` so Studio renders the overlay offline.

### Status indicators on the laptimer

- Status-bar REC label is gated on `cam_link_get_info().link_up`:
  - link down → label hidden
  - link up + idle → dim camera glyph
  - link up + recording → red `● REC`
- Settings → "Camera Module" row appears only when the link is up. Tap
  "Öffnen" → Video Settings screen with live preview canvas (640×360,
  5 Hz HW JPEG decode) for aiming + connection / SD / IP status.

See `cam-firmware/README.md` for the cam-side bring-up status.

---

# ROADMAP

> Living document — for any future Claude session in VS Code: read this
> section first to pick up the project state quickly. Architecture
> decisions and rationales are captured in the relevant commit messages
> (`git log --grep=cam_link` is a good entry point).

## ✅ Done — Laptimer side

- **GPS / OBD / CAN / Analog inputs** — production
- **LVGL UI** — 4-tile menu, timing dashboard with configurable zones,
  tracks/history/settings screens
- **WiFi AP + DNS captive-portal redirect** for the phone app
- **`data_server`** HTTP API: `/sessions`, `/session/<id>`, `/tracks`,
  `/track/<idx>`, `/track` (POST), `/videos`, `/video/<id>` (302 to cam)
- **Camera link (UART2 @ GPIO 30/31)** — non-blocking framed binary
  protocol (`SOF | TYPE | LEN-LE | PAYLOAD | CRC8/MAXIM`)
  - Auto REC START on `session_store_begin()`
  - Auto REC STOP on timing-screen back button
  - Forwards GPS @ 10 Hz, OBD @ 20 Hz, analog @ 10 Hz, lap markers
  - Decodes STATUS heartbeat (link health, REC state, IP, SD free)
  - Drops frames silently when no cam is connected — fail-safe
- **REC indicator** in status bar (auto-shown only when link_up)
- **Video Settings screen** (Settings → "Camera Module" → Öffnen)
  - Live preview canvas: HTTP `GET /preview.jpg` → HW JPEG decode → LVGL
    canvas, 5 Hz worker on Core 0
  - Status panel: connection / recording / SD free / resolution / IP
  - Row + screen are gated on `cam_link_get_info().link_up` — invisible
    if the cam is unplugged

## ✅ Done — Cam-firmware (`cam-firmware/`)

- **Project skeleton** for ESP-IDF, target `esp32p4`, 32 MB PSRAM,
  16 MB flash, 3 OTA slots
- **`cam_link/cam_link_protocol.h`** — byte-for-byte copy of the
  laptimer-side header (CI should diff)
- **`cam_link/cam_link_uart`** — UART RX framer + TX sender + STATUS
  heartbeat every 500 ms
- **`recorder/sd_mgr`** — SDMMC 4-bit mount, statvfs, `mkdir -p`
- **`recorder/avi_writer`** — recycled from main commit `e08cb92`,
  audio paths dropped, native FatFS + 500 MB f_expand pre-alloc + 16 KB
  cache-line-aligned DMA chunk buffer
- **`recorder/sidecar`** — NDJSON telemetry mirror (`gps`, `obd`, `ana`,
  `lap`, `session_start`, `session_end` events)
- **`recorder/recorder`** — orchestrates per-session directory layout
  `/sdcard/sessions/<id>/{video.avi, telemetry.ndjson, meta.json}`,
  scans the SD for the video index on demand
- **`wifi_sta`** — STA bring-up via `esp_hosted` 2.10.0 +
  `esp_wifi_remote` ~1.3.0, joins laptimer AP, exponential reconnect
- **`http_server`** — `/`, `/videos/list`, `/video/<id>`,
  `/telemetry/<id>`, `/preview.jpg`, with CORS + 30 s send-timeout
- **`capture/capture`** — V4L2 ioctl pipeline scaffold
  (`VIDIOC_DQBUF` → `recorder_push_jpeg_frame` + `stash_preview`),
  gated on `CONFIG_ESP_VIDEO_ENABLE` so the firmware still builds
  without the camera component installed
- **`idf_component.yml`** — pins `esp_hosted` 2.10.0 +
  `esp_wifi_remote` ~1.3.0 (matches main); declares `esp_video` +
  `esp_cam_sensor` for the upcoming OV5647 work

## ⏳ Open — needs hardware to verify

1. **MIPI-CSI bring-up** on the DFR1172 with the OV5647
   - Fill `esp_video_init_config_t` in `cam-firmware/main/capture/capture.c`
     with the DFR1172's CSI lane / sensor I2C / reset pins (board layout
     final once the PCB lands)
   - Verify `/dev/video0` appears, JPEG output at 1080p30
   - Reference: ESP-IDF `examples/peripherals/isp/multi_pipelines`
2. **End-to-end smoke test**
   - Laptimer triggers `REC START` → cam writes a playable AVI + matching
     NDJSON sidecar
   - Laptimer status bar flips to red `● REC`
   - Phone fetches `GET /videos` then `GET /video/<id>` (302 → cam IP)
3. **Preview verification**
   - Settings → "Camera Module" → Öffnen shows a live image at 5 Hz
   - "Kein Signal" overlay clears within ~500 ms of fetch start
4. **Field test** — full session on track, telemetry timestamps line up
   with video PTS within one frame (~33 ms)

## 🛣️ Future ideas (not committed to roadmap)

- Studio software (Windows) for SD-card-based offline analysis. Would
  consume `video.avi` + `telemetry.ndjson` directly from the cam SD.
- WiFi provisioning over UART so the cam can pair to a different SSID
  than the hard-coded `BRL-Laptimer` AP (currently in
  `cam-firmware/main/wifi_sta/wifi_sta.h`).
- Preview resolution toggle in Video Settings (cam supports 1280×720@60
  on OV5647 if we ever want a smoother aim view).
- OTA bridge: laptimer hosts cam firmware images, pushes via UART or
  cam-side HTTP `/ota` (currently flash via USB-C service port).
- Lap-marker auto-export: write a parallel `laps.json` on the cam SD that
  Studio can consume directly without parsing the NDJSON event stream.

## Key invariants — don't break these

- **Cam is optional.** Every `cam_link_*` send call must be non-blocking
  and silent on failure. UI elements that mention the cam must be gated
  on `cam_link_get_info().link_up`.
- **Laptimer SD stays in the device.** All "download my data" workflows
  must work either over WiFi or by removing the **cam** SD — never by
  pulling the laptimer SD.
- **Telemetry is mirrored, not split.** The cam writes a complete copy of
  the live stream so the offline (path 3) flow is self-contained.
- **`cam_link_protocol.h` is the contract.** Any change to enum values,
  payload structs, or framing must update both copies (laptimer's
  `main/camera_link/` and cam's `cam-firmware/main/cam_link/`) in the
  same commit, and bump `CAM_LINK_PROTO_VER`.
- **Bytes don't proxy through the laptimer for video.** Use 302 redirects
  from `data_server` to the cam's IP — the laptimer's WiFi stack must
  stay free for the timing/UI workload that the original 1-chip design
  starved (see commit `e08cb92` for the painful history).
