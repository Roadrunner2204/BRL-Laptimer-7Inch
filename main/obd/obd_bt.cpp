/**
 * obd_bt.cpp -- BLE client for BRL OBD Adapter (ESP-IDF NimBLE host API)
 *
 * Replaces NimBLE-Arduino with the ESP-IDF NimBLE host stack.
 * On ESP32-P4 the NimBLE host runs on the P4 while the BLE controller
 * runs on the ESP32-C6 co-processor (connected via SDIO / esp_hosted).
 * The Waveshare BSP configures the host-controller transport.
 *
 * Target  : "BRL OBD Adapter"
 * Our name: "BRL-Laptimer"
 *
 * Service  : 0000FFE0-0000-1000-8000-00805F9B34FB
 * CMD ch.  : 0000FFE1  Write  (Laptimer -> Adapter)
 * RESP ch. : 0000FFE2  Notify (Adapter -> Laptimer)
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
#include "compat.h"

#include <string.h>

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "obd_bt";

// ---------------------------------------------------------------------------
// UUIDs  (Bluetooth base UUID with 16-bit short IDs 0xFFE0, 0xFFE1, 0xFFE2)
// ---------------------------------------------------------------------------
static const ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xE0, 0xFF, 0x00, 0x00
);
static const ble_uuid128_t CMD_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xE1, 0xFF, 0x00, 0x00
);
static const ble_uuid128_t RESP_UUID = BLE_UUID128_INIT(
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0xE2, 0xFF, 0x00, 0x00
);

#define TARGET_NAME      "BRL OBD Adapter"
#define SCAN_DURATION_MS 5000
#define RETRY_INTERVAL   5000   // ms between reconnect attempts
#define REQ_TIMEOUT_MS   1000   // ms to wait for a PID response

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
static OBdBtState    s_state       = OBD_IDLE;
static uint16_t      s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t      s_cmd_val_handle  = 0;   // ATT handle for CMD char value
static uint16_t      s_resp_val_handle = 0;   // ATT handle for RESP char value
static uint16_t      s_resp_cccd_handle = 0;  // ATT handle for RESP CCCD
static uint32_t      s_retry_ts    = 0;
static uint32_t      s_req_ts      = 0;
static uint8_t       s_own_addr_type = 0;

// Peer address found during scan
static ble_addr_t    s_peer_addr;

// Service discovery bookkeeping
static bool          s_svc_found   = false;
static uint16_t      s_svc_start   = 0;
static uint16_t      s_svc_end     = 0;

// Response buffer -- written from NimBLE callback, read from poll()
static uint8_t       s_rx_buf[32]  = {};
static uint8_t       s_rx_len      = 0;
static volatile bool s_rx_ready    = false;

// NimBLE host sync flag
static volatile bool s_ble_synced  = false;

// PID round-robin
static const uint8_t PID_LIST[] = {
    0x0C,  // RPM          (2 B) -> (256A + B) / 4
    0x11,  // Throttle     (1 B) -> A * 100 / 255
    0x0B,  // MAP kPa      (1 B) -> A
    0x05,  // Coolant C    (1 B) -> A - 40
    0x0F,  // Intake C     (1 B) -> A - 40
};
static const uint8_t PID_COUNT = sizeof(PID_LIST);
static uint8_t s_pid_idx = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_scan(void);

// ---------------------------------------------------------------------------
// Parse response and update g_state.obd
// ---------------------------------------------------------------------------
static void apply_pid(uint8_t pid, const uint8_t *d, uint8_t len)
{
    ObdData &obd = g_state.obd;
    switch (pid) {
        case 0x0C: if (len >= 2) obd.rpm           = ((d[0] * 256u) + d[1]) / 4.0f; break;
        case 0x11: if (len >= 1) obd.throttle_pct  = d[0] * 100.0f / 255.0f;        break;
        case 0x0B: if (len >= 1) obd.boost_kpa     = (float)d[0];                    break;
        case 0x05: if (len >= 1) obd.coolant_temp_c = (float)d[0] - 40.0f;           break;
        case 0x0F: if (len >= 1) obd.intake_temp_c  = (float)d[0] - 40.0f;           break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Send one READ_PID request via GATT write-no-response
// ---------------------------------------------------------------------------
static void send_pid_request(uint8_t pid)
{
    if (s_cmd_val_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    uint8_t cmd[2] = { CMD_READ_PID, pid };
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_cmd_val_handle,
                                         cmd, sizeof(cmd));
    if (rc != 0) {
        ESP_LOGW(TAG, "Write CMD failed: rc=%d", rc);
        return;
    }
    s_rx_ready = false;
    s_req_ts   = millis();
    s_state    = OBD_REQUESTING;
}

// ---------------------------------------------------------------------------
// Disconnect helper
// ---------------------------------------------------------------------------
static void do_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    s_cmd_val_handle   = 0;
    s_resp_val_handle  = 0;
    s_resp_cccd_handle = 0;
    s_svc_found = false;
    g_state.obd.connected = false;
    g_state.obd_connected = false;
}

// ---------------------------------------------------------------------------
// Enable notifications on RESP characteristic by writing 0x0001 to its CCCD
// ---------------------------------------------------------------------------
static int on_subscribe_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to RESP notifications");
        g_state.obd.connected = true;
        g_state.obd_connected = true;
        s_pid_idx = 0;
        s_state   = OBD_CONNECTED;
        ESP_LOGI(TAG, "Connected to BRL OBD Adapter");
        // Send first PID request
        send_pid_request(PID_LIST[s_pid_idx]);
    } else {
        ESP_LOGE(TAG, "Subscribe failed: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
    }
    return 0;
}

static void subscribe_notifications(void)
{
    if (s_resp_cccd_handle == 0) {
        ESP_LOGE(TAG, "CCCD handle not found, cannot subscribe");
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return;
    }

    uint8_t val[2] = { 0x01, 0x00 };  // enable notifications
    int rc = ble_gattc_write_flat(s_conn_handle, s_resp_cccd_handle,
                                  val, sizeof(val), on_subscribe_cb, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD write failed: rc=%d", rc);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
    }
}

// ---------------------------------------------------------------------------
// GATT descriptor discovery callback -- locates the CCCD for RESP
// ---------------------------------------------------------------------------
static int gatt_dsc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        // All descriptors discovered -- now subscribe
        subscribe_notifications();
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Descriptor disc error: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return 0;
    }

    // Check if this descriptor is the CCCD (UUID 0x2902)
    static const ble_uuid16_t cccd_uuid = BLE_UUID16_INIT(0x2902);
    if (ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u) == 0) {
        s_resp_cccd_handle = dsc->handle;
        ESP_LOGI(TAG, "Found RESP CCCD handle: %d", s_resp_cccd_handle);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GATT characteristic discovery callback
// ---------------------------------------------------------------------------
static int gatt_chr_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        // All characteristics discovered
        if (s_cmd_val_handle == 0 || s_resp_val_handle == 0) {
            ESP_LOGE(TAG, "CMD or RESP characteristic not found");
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
            return 0;
        }

        ESP_LOGI(TAG, "CMD handle=%d, RESP handle=%d",
                 s_cmd_val_handle, s_resp_val_handle);

        // Discover descriptors on RESP characteristic to find its CCCD.
        // This is more robust than assuming CCCD is at val_handle+1.
        int rc = ble_gattc_disc_all_dscs(conn_handle,
                                         s_resp_val_handle,
                                         s_svc_end,
                                         gatt_dsc_disc_cb, nullptr);
        if (rc != 0) {
            ESP_LOGE(TAG, "Descriptor discovery failed: rc=%d", rc);
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Chr disc error: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return 0;
    }

    // Match CMD UUID
    if (ble_uuid_cmp(&chr->uuid.u, &CMD_UUID.u) == 0) {
        s_cmd_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found CMD characteristic, val_handle=%d", s_cmd_val_handle);
    }
    // Match RESP UUID
    if (ble_uuid_cmp(&chr->uuid.u, &RESP_UUID.u) == 0) {
        s_resp_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found RESP characteristic, val_handle=%d", s_resp_val_handle);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GATT service discovery callback
// ---------------------------------------------------------------------------
static int gatt_svc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        if (!s_svc_found) {
            ESP_LOGE(TAG, "Service 0xFFE0 not found");
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
            return 0;
        }
        // Discover characteristics within the service handle range
        int rc = ble_gattc_disc_all_chrs(conn_handle,
                                         s_svc_start, s_svc_end,
                                         gatt_chr_disc_cb, nullptr);
        if (rc != 0) {
            ESP_LOGE(TAG, "Chr discovery start failed: rc=%d", rc);
            do_disconnect();
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "Svc disc error: status=%d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        return 0;
    }

    // Check if this is our target service (0xFFE0)
    if (ble_uuid_cmp(&svc->uuid.u, &SERVICE_UUID.u) == 0) {
        s_svc_found = true;
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
        ESP_LOGI(TAG, "Found service FFE0: handles %d-%d", s_svc_start, s_svc_end);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GAP event callback -- handles scan, connect, disconnect, notify
// ---------------------------------------------------------------------------
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

        case BLE_GAP_EVENT_DISC: {
            // Scan result -- check device name in advertising data
            struct ble_hs_adv_fields fields;
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                             event->disc.length_data);
            if (rc != 0) break;

            if (fields.name != nullptr && fields.name_len > 0) {
                if (fields.name_len == strlen(TARGET_NAME) &&
                    memcmp(fields.name, TARGET_NAME, fields.name_len) == 0) {

                    ESP_LOGI(TAG, "Target found!");
                    s_peer_addr = event->disc.addr;
                    s_state = OBD_FOUND;
                    ble_gap_disc_cancel();
                }
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            // Scan window expired without finding the target
            if (s_state == OBD_SCANNING) {
                s_state    = OBD_IDLE;
                s_retry_ts = millis();
            }
            break;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected, conn_handle=%d",
                         event->connect.conn_handle);
                s_conn_handle = event->connect.conn_handle;

                // Reset discovery state
                s_svc_found = false;
                s_cmd_val_handle  = 0;
                s_resp_val_handle = 0;
                s_resp_cccd_handle = 0;

                // Start service discovery
                int rc = ble_gattc_disc_all_svcs(s_conn_handle,
                                                 gatt_svc_disc_cb, nullptr);
                if (rc != 0) {
                    ESP_LOGE(TAG, "Service discovery failed to start: rc=%d", rc);
                    do_disconnect();
                    s_state    = OBD_ERROR;
                    s_retry_ts = millis();
                }
            } else {
                ESP_LOGW(TAG, "Connect failed: status=%d", event->connect.status);
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_state    = OBD_ERROR;
                s_retry_ts = millis();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "Disconnected: reason=%d", event->disconnect.reason);
            s_conn_handle      = BLE_HS_CONN_HANDLE_NONE;
            s_cmd_val_handle   = 0;
            s_resp_val_handle  = 0;
            s_resp_cccd_handle = 0;
            s_svc_found = false;
            g_state.obd.connected = false;
            g_state.obd_connected = false;
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
            break;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            // Notification received from RESP characteristic
            if (event->notify_rx.attr_handle == s_resp_val_handle) {
                struct os_mbuf *om = event->notify_rx.om;
                uint16_t len = OS_MBUF_PKTLEN(om);
                if (len > sizeof(s_rx_buf)) len = sizeof(s_rx_buf);
                os_mbuf_copydata(om, 0, len, s_rx_buf);
                s_rx_len   = (uint8_t)len;
                s_rx_ready = true;
            }
            break;
        }

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated: conn_handle=%d mtu=%d",
                     event->mtu.conn_handle, event->mtu.value);
            break;

        default:
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NimBLE host task (runs in its own FreeRTOS task)
// ---------------------------------------------------------------------------
static void nimble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();           // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Called when the NimBLE host and controller are synced
// ---------------------------------------------------------------------------
static void on_sync(void)
{
    // Ensure we have a valid identity address
    ble_hs_util_ensure_addr(0);
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: rc=%d", rc);
    }
    s_ble_synced = true;
    ESP_LOGI(TAG, "BLE host synced -- ready to scan");
}

// ---------------------------------------------------------------------------
// Called on NimBLE host reset
// ---------------------------------------------------------------------------
static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
    s_ble_synced = false;
}

// ---------------------------------------------------------------------------
// Start BLE scan
// ---------------------------------------------------------------------------
static void start_scan(void)
{
    if (!s_ble_synced) return;

    struct ble_gap_disc_params disc_params = {};
    disc_params.filter_duplicates = 1;
    disc_params.passive           = 0;   // active scan
    // Low duty-cycle (10%): reduces BLE advertising-report processing and
    // the resulting heap churn that fragments DRAM.  With high duty-cycle
    // the DRAM heap becomes so fragmented that WiFi AP authentication timers
    // can no longer be allocated.
    disc_params.itvl              = BLE_GAP_SCAN_ITVL_MS(450);
    disc_params.window            = BLE_GAP_SCAN_WIN_MS(45);
    disc_params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;
    disc_params.limited           = 0;

    int rc = ble_gap_disc(s_own_addr_type, SCAN_DURATION_MS,
                          &disc_params, gap_event_cb, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed: rc=%d", rc);
        s_state    = OBD_IDLE;
        s_retry_ts = millis();
        return;
    }
    s_state = OBD_SCANNING;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void obd_bt_init(void)
{
    // Initialize NimBLE port
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Set device name for GAP
    ble_svc_gap_device_name_set("BRL-Laptimer");

    // Configure the NimBLE host
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Initialize the NimBLE store (required for pairing, even if unused)
    extern void ble_store_config_init(void);
    ble_store_config_init();

    // Start the NimBLE host task on its own FreeRTOS task
    nimble_port_freertos_init(nimble_host_task);

    s_state    = OBD_IDLE;
    s_retry_ts = 0;
    ESP_LOGI(TAG, "NimBLE init done");
}

void obd_bt_poll(void)
{
    const uint32_t now = millis();

    switch (s_state) {

        case OBD_IDLE:
            if (!s_ble_synced) break;  // wait for host sync
            if (now - s_retry_ts >= RETRY_INTERVAL) {
                start_scan();
            }
            break;

        case OBD_SCANNING:
            // gap_event_cb fires BLE_GAP_EVENT_DISC -> sets s_state = OBD_FOUND
            break;

        case OBD_FOUND: {
            s_state = OBD_CONNECTING;
            ESP_LOGI(TAG, "Connecting...");

            int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr,
                                     5000,     // connect timeout ms
                                     nullptr,  // default connection params
                                     gap_event_cb, nullptr);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_gap_connect failed: rc=%d", rc);
                s_state    = OBD_ERROR;
                s_retry_ts = now;
            }
            break;
        }

        case OBD_CONNECTING:
            // Waiting for BLE_GAP_EVENT_CONNECT callback
            break;

        case OBD_CONNECTED:
            if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                ESP_LOGW(TAG, "Connection lost in CONNECTED state");
                do_disconnect();
                s_state    = OBD_ERROR;
                s_retry_ts = now;
                break;
            }
            send_pid_request(PID_LIST[s_pid_idx]);
            break;

        case OBD_REQUESTING:
            if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
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
                // No response -- skip this PID, move on
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

OBdBtState obd_bt_state(void) { return s_state; }

void obd_bt_disconnect(void)
{
    do_disconnect();
    s_state    = OBD_IDLE;
    s_retry_ts = 0;
}
