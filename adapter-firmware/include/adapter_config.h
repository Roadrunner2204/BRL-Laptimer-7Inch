#pragma once
// =============================================================================
//  BMW OBD Adapter – Hardware-Konfiguration
//  Board: ESP32-S3 Super Mini (~22×18mm, USB-C, 4MB Flash, kein PSRAM)
// =============================================================================

// ── CAN Bus (SN65HVD230) ────────────────────────────────────────────────────
#define PIN_CAN_TX           1
#define PIN_CAN_RX           2      // + Deep Sleep Wakeup (ext0)
#define PIN_CAN_STBY         6      // RS-Pin Steuerung über MOSFET (GPIO3 ist Strapping!)

// ── K-Line (L9637D) ─────────────────────────────────────────────────────────
#define PIN_KLINE_TX         4
#define PIN_KLINE_RX         5

// ── Status LED ──────────────────────────────────────────────────────────────
#define PIN_LED              8

// ── CAN Baudraten ───────────────────────────────────────────────────────────
#define CAN_BAUD_DCAN       500000  // D-CAN / PT-CAN (500 kbit/s)
#define CAN_BAUD_KCAN       100000  // K-CAN (100 kbit/s)

// ── K-Line ──────────────────────────────────────────────────────────────────
#define KLINE_BAUD          10400   // ISO 9141-2 / KWP2000

// ── BMW Diagnose-Adressen ───────────────────────────────────────────────────
#define BMW_TESTER_ADDR     0x6F1
#define BMW_DME_RESP        0x612
#define OBD2_BROADCAST      0x7DF
#define OBD2_DME_REQ        0x7E0
#define OBD2_DME_RESP       0x7E8

// ── Timeouts ────────────────────────────────────────────────────────────────
#define ISOTP_FC_TIMEOUT_MS 1000
#define ISOTP_CF_TIMEOUT_MS 1000
#define UDS_RESP_TIMEOUT_MS 2000
#define KLINE_RESP_TIMEOUT  500

// ── BLE ─────────────────────────────────────────────────────────────────────
#define BLE_DEVICE_NAME     "BRL OBD Adapter"
