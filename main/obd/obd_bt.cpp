/**
 * obd_bt.cpp — BLE client for BRL OBD Adapter (ESP-IDF NimBLE)
 *
 * Ported from NimBLE-Arduino to ESP-IDF native NimBLE host API.
 * On ESP32-P4: BLE controller runs on ESP32-C6 co-processor via esp_hosted.
 * NimBLE host stack runs locally on ESP32-P4.
 *
 * Service  : 0000FFE0-0000-1000-8000-00805F9B34FB
 * CMD  ch. : 0000FFE1  Write  (Laptimer → Adapter)
 * RESP ch. : 0000FFE2  Notify (Adapter → Laptimer)
 */

#include "obd_bt.h"
#include "../data/lap_data.h"
#include "compat.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"

static const char *TAG = "obd_bt";

/* UUIDs */
static const ble_uuid128_t SERVICE_UUID =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xe0, 0xff, 0x00, 0x00);
static const ble_uuid128_t CMD_UUID =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xe1, 0xff, 0x00, 0x00);
static const ble_uuid128_t RESP_UUID =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0xe2, 0xff, 0x00, 0x00);

#define TARGET_NAME      "BRL OBD Adapter"
#define SCAN_DURATION_MS 5000
#define RETRY_INTERVAL   5000
#define REQ_TIMEOUT_MS   1000

/* Binary protocol */
enum OBDCmd : uint8_t  { CMD_READ_PID = 0x01, CMD_PING = 0xFF };
enum OBDStatus : uint8_t {
    STATUS_OK = 0x00, STATUS_TIMEOUT = 0x01, STATUS_NO_RESP = 0x02,
    STATUS_NEGATIVE = 0x03, STATUS_BUS_ERR = 0x04, STATUS_NOT_INIT = 0x05
};

/* State */
static OBdBtState  s_state      = OBD_IDLE;
static uint16_t    s_conn_handle = 0;
static uint16_t    s_cmd_val_handle = 0;
static uint16_t    s_resp_val_handle = 0;
static uint16_t    s_resp_cccd_handle = 0;
static uint32_t    s_retry_ts   = 0;
static uint32_t    s_req_ts     = 0;
static uint8_t     s_own_addr_type;
static ble_addr_t  s_target_addr;
static bool        s_target_found = false;

/* Response buffer */
static uint8_t       s_rx_buf[32] = {};
static uint8_t       s_rx_len     = 0;
static volatile bool s_rx_ready   = false;

/* PID round-robin */
static const uint8_t PID_LIST[] = { 0x0C, 0x11, 0x0B, 0x05, 0x0F };
static const uint8_t PID_COUNT = sizeof(PID_LIST);
static uint8_t s_pid_idx = 0;

/* Parse response and update g_state.obd */
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

/* Send one PID request */
static void send_pid_request(uint8_t pid) {
    if (s_cmd_val_handle == 0) return;
    uint8_t cmd[2] = { CMD_READ_PID, pid };
    ble_gattc_write_flat(s_conn_handle, s_cmd_val_handle, cmd, 2, NULL, NULL);
    s_rx_ready = false;
    s_req_ts   = millis();
    s_state    = OBD_REQUESTING;
}

/* Disconnect helper */
static void do_disconnect() {
    if (s_conn_handle) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = 0;
    }
    s_cmd_val_handle  = 0;
    s_resp_val_handle = 0;
    s_resp_cccd_handle = 0;
    g_state.obd.connected = false;
    g_state.obd_connected = false;
}

/* GATT notification callback */
static int on_notify(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0;
}

/* Notification event handler (called via GAP event) */
static void handle_notify(struct ble_gap_event *event) {
    if (event->notify_rx.attr_handle == s_resp_val_handle) {
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(s_rx_buf)) len = sizeof(s_rx_buf);
        os_mbuf_copydata(event->notify_rx.om, 0, len, s_rx_buf);
        s_rx_len   = (uint8_t)len;
        s_rx_ready = true;
    }
}

/* Subscribe to RESP notifications */
static int on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to notifications");
        g_state.obd.connected = true;
        g_state.obd_connected = true;
        s_pid_idx = 0;
        s_state   = OBD_CONNECTED;
    } else {
        ESP_LOGE(TAG, "Subscribe failed: %d", error->status);
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
    }
    return 0;
}

/* Characteristic discovery callback */
static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == BLE_HS_EDONE) {
        /* All characteristics discovered — subscribe to RESP */
        if (s_resp_val_handle) {
            uint8_t val[2] = {0x01, 0x00}; /* enable notifications */
            ble_gattc_write_flat(conn_handle, s_resp_val_handle + 1,
                                 val, 2, on_subscribe, NULL);
        }
        return 0;
    }
    if (error->status != 0) return 0;

    if (ble_uuid_cmp(&chr->uuid.u, &CMD_UUID.u) == 0) {
        s_cmd_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "CMD handle: %d", s_cmd_val_handle);
    }
    if (ble_uuid_cmp(&chr->uuid.u, &RESP_UUID.u) == 0) {
        s_resp_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "RESP handle: %d", s_resp_val_handle);
    }
    return 0;
}

/* Service discovery callback */
static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg) {
    if (error->status == BLE_HS_EDONE) return 0;
    if (error->status != 0) {
        ESP_LOGE(TAG, "Service discovery error: %d", error->status);
        do_disconnect();
        s_state = OBD_ERROR;
        s_retry_ts = millis();
        return 0;
    }

    if (ble_uuid_cmp(&svc->uuid.u, &SERVICE_UUID.u) == 0) {
        ESP_LOGI(TAG, "Service 0xFFE0 found");
        ble_gattc_disc_all_chrs(conn_handle, svc->start_handle,
                                 svc->end_handle, on_chr_disc, NULL);
    }
    return 0;
}

/* GAP event handler */
static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        /* Check if this is our target device */
        struct ble_hs_adv_fields fields;
        ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data);
        if (fields.name && fields.name_len > 0) {
            char name[32] = {};
            int len = fields.name_len < 31 ? fields.name_len : 31;
            memcpy(name, fields.name, len);
            if (strcmp(name, TARGET_NAME) == 0) {
                ESP_LOGI(TAG, "Target found: %s", name);
                s_target_addr = event->disc.addr;
                s_target_found = true;
                ble_gap_disc_cancel();
            }
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_target_found && s_state == OBD_SCANNING) {
            s_state    = OBD_IDLE;
            s_retry_ts = millis();
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected");
            s_conn_handle = event->connect.conn_handle;
            /* Discover services */
            ble_gattc_disc_all_svcs(s_conn_handle, on_svc_disc, NULL);
        } else {
            ESP_LOGE(TAG, "Connect failed: %d", event->connect.status);
            s_state    = OBD_ERROR;
            s_retry_ts = millis();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected: reason=%d", event->disconnect.reason);
        s_conn_handle = 0;
        do_disconnect();
        s_state    = OBD_ERROR;
        s_retry_ts = millis();
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        handle_notify(event);
        break;

    default:
        break;
    }
    return 0;
}

/* NimBLE host task */
static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Sync callback — NimBLE stack is ready */
static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ESP_LOGI(TAG, "NimBLE synced, ready to scan");
}

/* Public API */
void obd_bt_init() {
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = [](int reason) {
        ESP_LOGE(TAG, "NimBLE reset: reason=%d", reason);
    };

    nimble_port_freertos_init(ble_host_task);
    s_state    = OBD_IDLE;
    s_retry_ts = 0;
    ESP_LOGI(TAG, "NimBLE init done");
}

void obd_bt_poll() {
    const uint32_t now = millis();

    switch (s_state) {

    case OBD_IDLE:
        if (now - s_retry_ts >= RETRY_INTERVAL) {
            struct ble_gap_disc_params params = {};
            params.passive = 0;
            params.itvl = 450;
            params.window = 45;
            params.filter_duplicates = 1;
            s_target_found = false;
            int rc = ble_gap_disc(s_own_addr_type, SCAN_DURATION_MS,
                                  &params, gap_event_cb, NULL);
            if (rc == 0) {
                s_state = OBD_SCANNING;
            } else {
                s_retry_ts = now;
            }
        }
        break;

    case OBD_SCANNING:
        if (s_target_found) {
            s_target_found = false;
            int rc = ble_gap_connect(s_own_addr_type, &s_target_addr,
                                     5000, NULL, gap_event_cb, NULL);
            if (rc == 0) {
                s_state = OBD_CONNECTING;
            } else {
                ESP_LOGE(TAG, "Connect call failed: %d", rc);
                s_state    = OBD_ERROR;
                s_retry_ts = now;
            }
        }
        break;

    case OBD_CONNECTING:
        /* Waiting for GAP connect event */
        break;

    case OBD_CONNECTED:
        send_pid_request(PID_LIST[s_pid_idx]);
        break;

    case OBD_REQUESTING:
        if (s_rx_ready) {
            if (s_rx_len >= 2 &&
                s_rx_buf[0] == CMD_READ_PID &&
                s_rx_buf[1] == STATUS_OK) {
                apply_pid(PID_LIST[s_pid_idx], s_rx_buf + 2, s_rx_len - 2);
            }
            s_pid_idx = (s_pid_idx + 1) % PID_COUNT;
            s_state   = OBD_CONNECTED;
        } else if (now - s_req_ts > REQ_TIMEOUT_MS) {
            s_pid_idx = (s_pid_idx + 1) % PID_COUNT;
            s_state   = OBD_CONNECTED;
        }
        break;

    case OBD_ERROR:
        if (now - s_retry_ts >= RETRY_INTERVAL) {
            s_state    = OBD_IDLE;
            s_retry_ts = now;
        }
        break;

    default:
        break;
    }
}

OBdBtState obd_bt_state() { return s_state; }

void obd_bt_disconnect() {
    do_disconnect();
    s_state    = OBD_IDLE;
    s_retry_ts = 0;
}
