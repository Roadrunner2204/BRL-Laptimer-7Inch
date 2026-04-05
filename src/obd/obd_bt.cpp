/**
 * obd_bt.cpp — BLE client for BRL OBD Adapter
 *
 * Library : NimBLE-Arduino (h2zero/NimBLE-Arduino)
 * Target  : "BRL OBD Adapter"
 * Our name: "BRL-Laptimer"
 *
 * Service  : 0000FFE0-0000-1000-8000-00805F9B34FB
 * CMD  ch. : 0000FFE1  Write  (Laptimer → Adapter)
 * RESP ch. : 0000FFE2  Notify (Adapter → Laptimer)
 *
 * Binary protocol
 *   Send : [OBDCmd 1B] [payload...]
 *   Recv : [OBDCmd 1B] [OBDStatus 1B] [data bytes...]
 *
 * OBD-II mode 01 PIDs requested:
 *   0x0C RPM, 0x11 Throttle, 0x0B MAP, 0x05 Coolant, 0x0F Intake
 */

#include "obd_bt.h"
#include "../data/lap_data.h"
#include <Arduino.h>
#include <NimBLEDevice.h>

// ---------------------------------------------------------------------------
// UUIDs & constants
// ---------------------------------------------------------------------------
static NimBLEUUID SERVICE_UUID("0000FFE0-0000-1000-8000-00805F9B34FB");
static NimBLEUUID CMD_UUID    ("0000FFE1-0000-1000-8000-00805F9B34FB");
static NimBLEUUID RESP_UUID   ("0000FFE2-0000-1000-8000-00805F9B34FB");

#define TARGET_NAME     "BRL OBD Adapter"
#define SCAN_SECONDS    5
#define RETRY_INTERVAL  5000   // ms between reconnect attempts
#define REQ_TIMEOUT_MS  1000   // ms to wait for a PID response

// ---------------------------------------------------------------------------
// Binary protocol enums
// ---------------------------------------------------------------------------
enum OBDCmd : uint8_t {
    CMD_READ_PID = 0x01,
    CMD_PING     = 0xFF,
};
enum OBDStatus : uint8_t {
    STATUS_OK       = 0x00,
    STATUS_TIMEOUT  = 0x01,
    STATUS_NO_RESP  = 0x02,
    STATUS_NEGATIVE = 0x03,
    STATUS_BUS_ERR  = 0x04,
    STATUS_NOT_INIT = 0x05,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static OBdBtState                     s_state      = OBD_IDLE;
static NimBLEAddress                  s_addr;
static NimBLEClient                  *s_client     = nullptr;
static NimBLERemoteCharacteristic    *s_cmd_char   = nullptr;
static NimBLEScan                    *s_scan       = nullptr;
static uint32_t                       s_retry_ts   = 0;
static uint32_t                       s_req_ts     = 0;

// Response buffer — written from NimBLE notify task, read from logic_task
static uint8_t          s_rx_buf[32] = {};
static uint8_t          s_rx_len     = 0;
static volatile bool    s_rx_ready   = false;

// PID round-robin
static const uint8_t PID_LIST[] = {
    0x0C,  // RPM          (2 B) → (256A + B) / 4
    0x11,  // Throttle     (1 B) → A × 100 / 255
    0x0B,  // MAP kPa      (1 B) → A
    0x05,  // Coolant °C   (1 B) → A − 40
    0x0F,  // Intake °C    (1 B) → A − 40
};
static const uint8_t PID_COUNT = sizeof(PID_LIST);
static uint8_t s_pid_idx = 0;

// ---------------------------------------------------------------------------
// RESP notify callback (called from NimBLE stack task)
// ---------------------------------------------------------------------------
static void on_resp_notify(NimBLERemoteCharacteristic * /*c*/,
                           uint8_t *data, size_t len, bool /*isNotify*/) {
    if (len > sizeof(s_rx_buf)) len = sizeof(s_rx_buf);
    memcpy(s_rx_buf, data, len);
    s_rx_len   = (uint8_t)len;
    s_rx_ready = true;   // signal to poll()
}

// ---------------------------------------------------------------------------
// BLE scan callbacks (called from NimBLE stack task)
// ---------------------------------------------------------------------------
static void on_scan_complete(NimBLEScanResults results);

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *dev) override {
        // Log every device found (helps diagnose name mismatches)
        log_e("[OBD] BLE found: '%s'  addr=%s",
              dev->getName().c_str(),
              dev->getAddress().toString().c_str());
        if (dev->getName() == TARGET_NAME) {
            log_e("[OBD] Target found, stopping scan");
            s_addr  = dev->getAddress();
            s_state = OBD_FOUND;
            NimBLEDevice::getScan()->stop();
        }
    }
};
static ScanCB s_scan_cb;

// Called when scan window expires (device not found → retry)
static void on_scan_complete(NimBLEScanResults /*results*/) {
    if (s_state == OBD_SCANNING) {
        log_e("[OBD] Scan complete, target not found — will retry");
        s_state    = OBD_IDLE;
        s_retry_ts = millis();
    }
}

// ---------------------------------------------------------------------------
// Parse response and update g_state.obd
// ---------------------------------------------------------------------------
static void apply_pid(uint8_t pid, const uint8_t *d, uint8_t len) {
    ObdData &obd = g_state.obd;
    switch (pid) {
        case 0x0C: if (len >= 2) obd.rpm           = ((d[0] * 256u) + d[1]) / 4.0f; break;
        case 0x11: if (len >= 1) obd.throttle_pct  = d[0] * 100.0f / 255.0f;        break;
        case 0x0B: if (len >= 1) obd.boost_kpa     = (float)d[0];                    break;
        case 0x05: if (len >= 1) obd.coolant_temp_c = (float)d[0] - 40.0f;          break;
        case 0x0F: if (len >= 1) obd.intake_temp_c  = (float)d[0] - 40.0f;          break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Send one READ_PID request
// ---------------------------------------------------------------------------
static void send_pid_request(uint8_t pid) {
    if (!s_cmd_char) return;
    uint8_t cmd[2] = { CMD_READ_PID, pid };
    s_cmd_char->writeValue(cmd, 2, false);  // false = write without response
    s_rx_ready = false;
    s_req_ts   = millis();
    s_state    = OBD_REQUESTING;
}

// ---------------------------------------------------------------------------
// Disconnect helper
// ---------------------------------------------------------------------------
static void do_disconnect() {
    s_cmd_char = nullptr;
    if (s_client) {
        if (s_client->isConnected()) s_client->disconnect();
        NimBLEDevice::deleteClient(s_client);
        s_client = nullptr;
    }
    g_state.obd.connected = false;
    g_state.obd_connected = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void obd_bt_init() {
    NimBLEDevice::init("BRL-Laptimer");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    s_scan = NimBLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_scan_cb, false);
    s_scan->setActiveScan(true);
    // Low duty-cycle (10%): reduces BLE advertising-report processing and the
    // resulting heap churn that fragments DRAM.  With 99% duty-cycle the DRAM
    // heap becomes so fragmented that WiFi AP authentication timers (~60 B)
    // can no longer be allocated -> ESP_ERR_NO_MEM -> abort().
    s_scan->setInterval(450);   // was 100 ms
    s_scan->setWindow(45);      // 45/450 = 10% duty cycle (was 99/100 = 99%)
    s_scan->setDuplicateFilter(true);  // process each device only once per scan
    s_state    = OBD_IDLE;
    s_retry_ts = 0;
    log_e("[OBD] NimBLE init done");
}

void obd_bt_poll() {
    const uint32_t now = millis();

    switch (s_state) {

        case OBD_IDLE:
            if (now - s_retry_ts >= RETRY_INTERVAL) {
                log_e("[OBD] Starting BLE scan...");
                s_scan->clearResults();
                s_scan->start(SCAN_SECONDS, on_scan_complete, false);
                s_state = OBD_SCANNING;
            }
            break;

        case OBD_SCANNING:
            // ScanCB::onResult() fires from NimBLE task → sets s_state = OBD_FOUND
            break;

        case OBD_FOUND: {
            s_client = NimBLEDevice::createClient();
            s_client->setConnectTimeout(5);  // seconds

            log_e("[OBD] Connecting...");
            if (!s_client->connect(s_addr)) {
                log_e("[OBD] Connect failed");
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            NimBLERemoteService *svc = s_client->getService(SERVICE_UUID);
            if (!svc) {
                log_e("[OBD] Service 0xFFE0 not found");
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            s_cmd_char = svc->getCharacteristic(CMD_UUID);
            NimBLERemoteCharacteristic *resp = svc->getCharacteristic(RESP_UUID);
            if (!s_cmd_char || !resp) {
                log_e("[OBD] Characteristics not found");
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            if (!resp->subscribe(true, on_resp_notify)) {
                log_e("[OBD] Subscribe failed");
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            g_state.obd.connected = true;
            g_state.obd_connected = true;
            s_pid_idx = 0;
            s_state   = OBD_CONNECTED;
            log_e("[OBD] Connected to BRL OBD Adapter");
            send_pid_request(PID_LIST[s_pid_idx]);
            break;
        }

        case OBD_CONNECTED:
            if (!s_client || !s_client->isConnected()) {
                log_e("[OBD] Disconnected");
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            send_pid_request(PID_LIST[s_pid_idx]);
            break;

        case OBD_REQUESTING:
            if (!s_client || !s_client->isConnected()) {
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            if (s_rx_ready) {
                // Response: [CMD=0x01] [STATUS] [data bytes...]
                if (s_rx_len >= 2 &&
                    s_rx_buf[0] == CMD_READ_PID &&
                    s_rx_buf[1] == STATUS_OK) {
                    apply_pid(PID_LIST[s_pid_idx], s_rx_buf + 2, s_rx_len - 2);
                }
                s_pid_idx = (s_pid_idx + 1) % PID_COUNT;
                s_state   = OBD_CONNECTED;
            } else if (now - s_req_ts > REQ_TIMEOUT_MS) {
                // No response — skip this PID
                s_pid_idx = (s_pid_idx + 1) % PID_COUNT;
                s_state   = OBD_CONNECTED;
            }
            break;

        case OBD_ERROR:
            // do_disconnect() was already called when entering ERROR state
            if (now - s_retry_ts >= RETRY_INTERVAL) {
                s_state    = OBD_IDLE;
                s_retry_ts = now;
            }
            break;
    }
}

OBdBtState obd_bt_state() { return s_state; }

void obd_bt_disconnect() {
    do_disconnect();
    s_state    = OBD_IDLE;
    s_retry_ts = 0;
}
