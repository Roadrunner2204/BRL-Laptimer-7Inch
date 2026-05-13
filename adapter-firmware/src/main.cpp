// =============================================================================
//  Bavarian-RaceLabs OBD Adapter – ESP32-S3 Super Mini
//  BLE GATT Server → Display
// =============================================================================
#include <Arduino.h>
#include "adapter_config.h"
#include "isotp.h"
#include "uds_client.h"
#include "kline.h"
#include "ble_server.h"
#include "elm_emulator.h"
#include "driver/twai.h"

// ── Globale Objekte ─────────────────────────────────────────────────────────
IsotpHandler isotp;
UdsClient    uds(isotp);
KLineHandler kline;
BleServer    ble;
ElmEmulator  elm(uds, isotp);

// ── Status ──────────────────────────────────────────────────────────────────
enum class BusType : uint8_t { NONE, CAN_500K, CAN_100K, KLINE };
static BusType    activeBus    = BusType::NONE;
static uint32_t   activeTxId   = OBD2_DME_REQ;
static uint32_t   activeRxId   = OBD2_DME_RESP;
static bool       canInstalled = false;

// ── Diagnose-Session ────────────────────────────────────────────────────────
// Standard-OBD2 Extended Diagnostic Session (0x10 0x03). Wird vor Mode-22
// DID-Reads (z.B. Mode-09 Vehicle-Info-DIDs) automatisch gesetzt und für
// 4 Sekunden gecacht — solange schickt der Caller weiter Anfragen ohne
// dass jedes Mal eine neue Session etabliert werden muss.
static uint32_t   diagSessionTxId  = 0;
static uint32_t   diagSessionTime  = 0;
static const uint32_t DIAG_SESSION_REFRESH_MS = 4000;

static bool ensureDiagSession(uint32_t txId, uint32_t rxId) {
    uint32_t now = millis();
    if (txId == diagSessionTxId
        && (now - diagSessionTime) < DIAG_SESSION_REFRESH_MS)
        return true;
    if (uds.startDiagSession(0x03, txId, rxId)) {
        diagSessionTxId = txId;
        diagSessionTime = now;
        return true;
    }
    return false;
}

// ── CAN Bus ─────────────────────────────────────────────────────────────────
static bool canInit(uint32_t baudRate) {
    if (canInstalled) {
        twai_stop();
        twai_driver_uninstall();
        canInstalled = false;
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 32;
    g_config.tx_queue_len = 8;

    twai_timing_config_t t_config;
    if (baudRate == CAN_BAUD_KCAN)
        t_config = TWAI_TIMING_CONFIG_100KBITS();
    else
        t_config = TWAI_TIMING_CONFIG_500KBITS();

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        Serial.println("[CAN] Driver install failed");
        return false;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("[CAN] Start failed");
        twai_driver_uninstall();
        return false;
    }

    canInstalled = true;
    Serial.printf("[CAN] Bus gestartet: %d kbit/s\n", baudRate / 1000);
    return true;
}


// ── BLE Kommando-Handler ────────────────────────────────────────────────────
static void handleBleCommand(OBDCmd cmd, const uint8_t* data, uint8_t len) {
    UdsResponse resp;

    switch (cmd) {
        case OBDCmd::READ_PID: {
            if (len < 1) { ble.sendResponse(cmd, OBDStatus::ERR_NO_RESP); return; }
            uint8_t pid = data[0];
            if (activeBus == BusType::KLINE) {
                uint8_t pidData[8]; uint8_t pidLen = 0;
                if (kline.readPidKLine(pid, pidData, &pidLen))
                    ble.sendResponse(cmd, OBDStatus::OK, pidData, pidLen);
                else
                    ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            } else {
                if (uds.readPid(pid, resp, activeTxId, activeRxId))
                    ble.sendResponse(cmd, OBDStatus::OK, resp.data, resp.len);
                else
                    ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            }
            break;
        }

        case OBDCmd::READ_MULTI_PID: {
            if (len < 1 || len > 6) { ble.sendResponse(cmd, OBDStatus::ERR_NO_RESP); return; }
            if (activeBus == BusType::KLINE) {
                ble.sendResponse(cmd, OBDStatus::ERR_NO_RESP);
            } else {
                if (uds.readMultiPid(data, len, resp, activeTxId, activeRxId))
                    ble.sendResponse(cmd, OBDStatus::OK, resp.data, resp.len);
                else
                    ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            }
            break;
        }

        case OBDCmd::DISCOVER_PIDS: {
            // Mode-01 PIDs 0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0 sind die
            // "supported PIDs" Anker-Bytes — jeder gibt eine 4-byte Bitmap der
            // nächsten 32 PIDs zurück (MSB = niedrigster PID in dieser Gruppe).
            //
            // Resulting 28-byte Output-Bitmap (PIDs 0x01..0xE0, MSB-first):
            //   byte[0]: bit7=PID 0x01, bit6=PID 0x02, ..., bit0=PID 0x08
            //   byte[3]: bit7=PID 0x19, ..., bit0=PID 0x20  ← anchor 0x20
            //   byte[27]: bit7=PID 0xD9, ..., bit0=PID 0xE0
            //
            // Wenn das Auto auf 0x00 nicht antwortet → keine OBD2-Unterstützung.
            // Wenn 0x00 antwortet aber bit für 0x20 = 0 → keine PIDs in 0x21+.
            // → wir hören dann auf weiter zu fragen (spart ~1 Sekunde).
            if (activeBus == BusType::NONE || activeBus == BusType::KLINE) {
                ble.sendResponse(cmd, OBDStatus::ERR_NOT_INIT);
                break;
            }
            uint8_t bitmap[28] = {0};
            uint8_t anchors[7] = {0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0};
            uint8_t found_groups = 0;
            for (int g = 0; g < 7; g++) {
                if (g > 0) {
                    // Vorheriger Anchor muss gesagt haben dass dieser hier supported ist
                    // (bit 0 des letzten Bytes der vorherigen Gruppe).
                    int prev_byte = (g - 1) * 4 + 3;   // Index des Anchor-Bits im Bitmap
                    if ((bitmap[prev_byte] & 0x01) == 0) break;
                }
                if (uds.readPid(anchors[g], resp, activeTxId, activeRxId)
                    && resp.len >= 6
                    && resp.data[0] == 0x41
                    && resp.data[1] == anchors[g]) {
                    // 4 Daten-Bytes ab Position 2
                    bitmap[g * 4 + 0] = resp.data[2];
                    bitmap[g * 4 + 1] = resp.data[3];
                    bitmap[g * 4 + 2] = resp.data[4];
                    bitmap[g * 4 + 3] = resp.data[5];
                    found_groups++;
                } else {
                    break;   // ECU antwortet nicht mehr → fertig
                }
            }
            Serial.printf("[DISCOVER] %d Gruppen gefüllt — Bitmap: ", (int)found_groups);
            for (int i = 0; i < 28; i++) Serial.printf("%02X ", bitmap[i]);
            Serial.println();
            ble.sendResponse(cmd, OBDStatus::OK, bitmap, sizeof(bitmap));
            break;
        }

        case OBDCmd::READ_VIN: {
            char vin[18] = {};
            if (uds.readVin(vin, sizeof(vin)))
                ble.sendResponse(cmd, OBDStatus::OK, (const uint8_t*)vin, strlen(vin));
            else
                ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            break;
        }

        case OBDCmd::READ_DTC:
            if (uds.readDtcsObd2(resp))
                ble.sendResponse(cmd, OBDStatus::OK, resp.data, resp.len);
            else
                ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            break;

        case OBDCmd::CLEAR_DTC:
            if (uds.clearDtcsObd2())
                ble.sendResponse(cmd, OBDStatus::OK);
            else
                ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            break;

        case OBDCmd::READ_DID_22: {
            if (len < 2) { ble.sendResponse(cmd, OBDStatus::ERR_NO_RESP); return; }
            uint16_t did = ((uint16_t)data[0] << 8) | data[1];
            ensureDiagSession(activeTxId, activeRxId);
            if (uds.readDid(did, resp, activeTxId, activeRxId))
                ble.sendResponse(cmd, OBDStatus::OK, resp.data, resp.len);
            else {
                OBDStatus st = resp.negative ? OBDStatus::ERR_NEGATIVE : OBDStatus::ERR_TIMEOUT;
                ble.sendResponse(cmd, st, &resp.nrc, resp.negative ? 1 : 0);
            }
            break;
        }


        case OBDCmd::READ_DTC_UDS:
            ensureDiagSession(activeTxId, activeRxId);
            if (uds.readDtcsUds(resp, activeTxId, activeRxId))
                ble.sendResponse(cmd, OBDStatus::OK, resp.data, resp.len);
            else
                ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            break;

        case OBDCmd::CLEAR_DTC_UDS:
            ensureDiagSession(activeTxId, activeRxId);
            if (uds.clearDtcsUds(activeTxId, activeRxId))
                ble.sendResponse(cmd, OBDStatus::OK);
            else
                ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            break;

        case OBDCmd::DIAG_SESSION: {
            uint8_t session = (len >= 1) ? data[0] : 0x01;
            if (uds.startDiagSession(session, activeTxId, activeRxId))
                ble.sendResponse(cmd, OBDStatus::OK);
            else
                ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            break;
        }

        case OBDCmd::SET_BUS_CAN: {
            uint32_t baud = (len >= 1 && data[0] == 1) ? CAN_BAUD_KCAN : CAN_BAUD_DCAN;
            if (canInit(baud)) {
                activeBus = (baud == CAN_BAUD_KCAN) ? BusType::CAN_100K : BusType::CAN_500K;
                ble.sendResponse(cmd, OBDStatus::OK);
            } else {
                ble.sendResponse(cmd, OBDStatus::ERR_BUS);
            }
            break;
        }

        case OBDCmd::SET_BUS_KLINE:
            if (kline.begin(PIN_KLINE_TX, PIN_KLINE_RX)) {
                if (kline.fastInit() || kline.slowInit()) {
                    activeBus = BusType::KLINE;
                    ble.sendResponse(cmd, OBDStatus::OK);
                } else {
                    ble.sendResponse(cmd, OBDStatus::ERR_BUS);
                }
            } else {
                ble.sendResponse(cmd, OBDStatus::ERR_BUS);
            }
            break;

        case OBDCmd::GET_STATUS: {
            uint8_t status[3] = {
                0x01, (uint8_t)activeBus,
                (uint8_t)(activeBus != BusType::NONE ? 1 : 0)
            };
            ble.sendResponse(cmd, OBDStatus::OK, status, 3);
            break;
        }

        case OBDCmd::PING:
            ble.sendResponse(cmd, OBDStatus::OK);
            break;

        default:
            ble.sendResponse(cmd, OBDStatus::ERR_NOT_INIT);
            break;
    }
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(3000);  // USB CDC: warten bis Host den Port erkennt

    Serial.println();
    Serial.println("==================================");
    Serial.println("  BRL OBD Adapter v1.1");
    Serial.println("  Universal OBD-II (Mode 01/03/09)");
    Serial.println("  ESP32-S3 Super Mini");
    Serial.println("==================================");
    Serial.flush();

    // LED
    Serial.println("[BOOT] LED init...");
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    // CAN Standby Pin
    Serial.println("[BOOT] CAN STBY init...");
    pinMode(PIN_CAN_STBY, OUTPUT);
    digitalWrite(PIN_CAN_STBY, HIGH);

    // BLE Server starten
    Serial.println("[BOOT] BLE init...");
    Serial.flush();
    ble.begin(handleBleCommand);
    Serial.println("[BOOT] BLE OK");
    Serial.flush();

    ble.setDisconnectCb([]() {
        diagSessionTxId = 0;
        activeTxId      = OBD2_DME_REQ;
        activeRxId      = OBD2_DME_RESP;
    });

    // ELM327/329 Emulator: NUS → ELM Parser → CAN → NUS
    ble.setNusCallback([](const uint8_t* data, uint16_t len) {
        elm.feed(data, len);
    });
    elm.setSendCallback([](const char* data, uint16_t len) {
        ble.nusSend(data, len);
    });
    elm.setBusInitCallback([](uint32_t baudRate) -> bool {
        return canInit(baudRate);
    });

    Serial.println("[MAIN] ELM327/329 Emulator bereit (NUS Service)");

    // Auto-Init CAN @ 500k beim Boot. Der BRL-Laptimer schickt kein
    // SET_BUS_CAN — ohne diese Zeile wäre der TWAI-Treiber nicht
    // installiert wenn die erste Mode-01-Anfrage kommt. 500 kBit/s deckt
    // den OBD-II-Diagnose-Bus von ~99% aller Autos ab Modelljahr 2008 ab
    // (CAN-OBD ist seit 2008 in EU/USA Pflicht).
    Serial.println("[BOOT] Auto-Init CAN @ 500k...");
    if (canInit(CAN_BAUD_DCAN)) {
        activeBus = BusType::CAN_500K;
        Serial.println("[BOOT] CAN ready");
    } else {
        Serial.println("[BOOT] CAN init failed — wird bei SET_BUS_CAN nochmal versucht");
    }

    Serial.println("[MAIN] Bereit – warte auf BLE-Verbindung...");
    Serial.flush();
    digitalWrite(PIN_LED, LOW);
}

// ── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // BLE-Befehle aus Queue verarbeiten (BRL Binary + ELM327 NUS)
    ble.processCommandQueue();
    ble.processNusQueue();
    elm.update();

    // LED: An wenn BLE verbunden
    digitalWrite(PIN_LED, ble.isConnected() ? HIGH : LOW);

    // K-Line Keepalive
    if (activeBus == BusType::KLINE && kline.isConnected())
        kline.keepAlive();

    delay(1);
}
