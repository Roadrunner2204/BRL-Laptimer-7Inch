/**
 * obd_bt.cpp — BLE client for BRL OBD Adapter
 *
 * Uses Arduino ESP32 BLE library (built-in with espressif32 platform).
 * Nordic UART Service UUIDs:
 *   Service : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   TX char : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (write — send to adapter)
 *   RX char : 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (notify — recv from adapter)
 */

#include "obd_bt.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLERemoteCharacteristic.h>

// ---------------------------------------------------------------------------
// NUS UUIDs
// ---------------------------------------------------------------------------
static BLEUUID NUS_SERVICE_UUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID NUS_TX_UUID     ("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID NUS_RX_UUID     ("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");

#define TARGET_NAME     "BRL-OBD"
#define SCAN_SECONDS    5
#define RETRY_INTERVAL  5000   // ms between reconnect attempts

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static OBdBtState          s_state        = OBD_IDLE;
static BLEAddress          s_addr         = BLEAddress("00:00:00:00:00:00");
static BLEClient          *s_client       = nullptr;
static BLERemoteCharacteristic *s_tx_char = nullptr;  // write to adapter
static BLEScan            *s_scan         = nullptr;
static uint32_t            s_retry_ts     = 0;

// Response buffer (NUS notify callback fills this)
static char     s_rx_buf[64]  = {};
static uint8_t  s_rx_len      = 0;
static bool     s_rx_ready    = false;

// PID request round-robin
static const uint8_t PID_LIST[] = {
    0x0C,  // RPM
    0x11,  // throttle
    0x0B,  // MAP/boost
    0x05,  // coolant
    0x0F,  // intake temp
};
static const uint8_t PID_COUNT = sizeof(PID_LIST);
static uint8_t s_pid_idx = 0;

// ---------------------------------------------------------------------------
// NUS RX notification callback
// ---------------------------------------------------------------------------
static void on_rx_notify(BLERemoteCharacteristic * /*c*/, uint8_t *data,
                          size_t length, bool /*isNotify*/) {
    if (length >= sizeof(s_rx_buf)) length = sizeof(s_rx_buf) - 1;
    memcpy(s_rx_buf, data, length);
    s_rx_buf[length] = '\0';
    s_rx_len   = (uint8_t)length;
    s_rx_ready = true;
}

// ---------------------------------------------------------------------------
// BLE scan callback
// ---------------------------------------------------------------------------
class ScanCB : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (dev.getName() == TARGET_NAME) {
            Serial.printf("[OBD] Found %s\n", TARGET_NAME);
            s_addr  = dev.getAddress();
            s_state = OBD_FOUND;
            dev.getScan()->stop();
        }
    }
};
static ScanCB s_scan_cb;

// ---------------------------------------------------------------------------
// OBD parser helpers
// ---------------------------------------------------------------------------
// Parse "41 XX AA BB" response for 1 or 2 byte values
static bool parse_pid_response(const char *buf, uint8_t expected_pid,
                                uint8_t &a, uint8_t &b) {
    uint8_t mode, pid;
    int aa, bb;
    // Try 2-byte format first
    if (sscanf(buf, "%hhx %hhx %x %x", &mode, &pid, &aa, &bb) == 4 &&
        mode == 0x41 && pid == expected_pid) {
        a = (uint8_t)aa; b = (uint8_t)bb; return true;
    }
    if (sscanf(buf, "%hhx %hhx %x", &mode, &pid, &aa) == 3 &&
        mode == 0x41 && pid == expected_pid) {
        a = (uint8_t)aa; b = 0; return true;
    }
    return false;
}

static void apply_pid(uint8_t pid, uint8_t a, uint8_t b) {
    ObdData &obd = g_state.obd;
    switch (pid) {
        case 0x0C: obd.rpm           = ((float)((a * 256) + b)) / 4.0f; break;
        case 0x11: obd.throttle_pct  = a * 100.0f / 255.0f;             break;
        case 0x0B: obd.boost_kpa     = (float)a;                         break;
        case 0x05: obd.coolant_temp_c = (float)a - 40.0f;               break;
        case 0x0F: obd.intake_temp_c  = (float)a - 40.0f;               break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Send one OBD request
// ---------------------------------------------------------------------------
static void request_next_pid() {
    if (!s_tx_char) return;
    char cmd[12];
    snprintf(cmd, sizeof(cmd), "01 %02X\r", PID_LIST[s_pid_idx]);
    s_tx_char->writeValue((uint8_t *)cmd, strlen(cmd));
    s_state    = OBD_REQUESTING;
    s_rx_ready = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void obd_bt_init() {
    BLEDevice::init("BRL-Laptimer");
    s_scan = BLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_scan_cb, false);
    s_scan->setActiveScan(true);
    s_state = OBD_IDLE;
    Serial.println("[OBD] BLE init done");
}

void obd_bt_poll() {
    uint32_t now = millis();

    switch (s_state) {

        case OBD_IDLE:
            if (now - s_retry_ts >= RETRY_INTERVAL) {
                Serial.println("[OBD] Starting BLE scan...");
                s_scan->start(SCAN_SECONDS, false);
                s_state = OBD_SCANNING;
            }
            break;

        case OBD_SCANNING:
            // onResult() fires in scan callback — moves to FOUND
            break;

        case OBD_FOUND:
            s_client = BLEDevice::createClient();
            Serial.printf("[OBD] Connecting to %s\n", s_addr.toString().c_str());
            if (!s_client->connect(s_addr)) {
                Serial.println("[OBD] Connect failed");
                delete s_client; s_client = nullptr;
                s_state = OBD_ERROR;
                break;
            }
            {
                BLERemoteService *svc = s_client->getService(NUS_SERVICE_UUID);
                if (!svc) {
                    Serial.println("[OBD] NUS service not found");
                    s_client->disconnect();
                    s_state = OBD_ERROR;
                    break;
                }
                s_tx_char = svc->getCharacteristic(NUS_TX_UUID);
                BLERemoteCharacteristic *rx = svc->getCharacteristic(NUS_RX_UUID);
                if (!s_tx_char || !rx) {
                    Serial.println("[OBD] NUS chars not found");
                    s_client->disconnect();
                    s_state = OBD_ERROR;
                    break;
                }
                rx->registerForNotify(on_rx_notify);
                g_state.obd.connected = true;
                g_state.obd_connected = true;
                s_state = OBD_CONNECTED;
                s_pid_idx = 0;
                Serial.println("[OBD] Connected to BRL-OBD");
                request_next_pid();
            }
            break;

        case OBD_CONNECTED:
            if (!s_client || !s_client->isConnected()) {
                Serial.println("[OBD] Disconnected");
                g_state.obd.connected = false;
                g_state.obd_connected = false;
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            request_next_pid();
            break;

        case OBD_REQUESTING:
            if (!s_client || !s_client->isConnected()) {
                s_state = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            if (s_rx_ready) {
                // Parse response
                uint8_t a = 0, b = 0;
                if (parse_pid_response(s_rx_buf, PID_LIST[s_pid_idx], a, b)) {
                    apply_pid(PID_LIST[s_pid_idx], a, b);
                }
                s_pid_idx = (s_pid_idx + 1) % PID_COUNT;
                s_state   = OBD_CONNECTED;
            }
            break;

        case OBD_ERROR:
            g_state.obd.connected = false;
            g_state.obd_connected = false;
            if (now - s_retry_ts >= RETRY_INTERVAL) {
                s_state    = OBD_IDLE;
                s_retry_ts = now;
            }
            break;
    }
}

OBdBtState obd_bt_state() { return s_state; }

void obd_bt_disconnect() {
    if (s_client && s_client->isConnected()) s_client->disconnect();
    g_state.obd.connected = false;
    g_state.obd_connected = false;
    s_state = OBD_IDLE;
}
