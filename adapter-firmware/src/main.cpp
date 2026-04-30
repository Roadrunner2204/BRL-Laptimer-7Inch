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
static uint32_t   diagSessionTxId  = 0;
static uint32_t   diagSessionTime  = 0;
static const uint32_t DIAG_SESSION_REFRESH_MS = 4000;

static bool ensureDiagSession(uint32_t txId, uint32_t rxId) {
    uint32_t now = millis();
    if (txId == diagSessionTxId && (now - diagSessionTime) < DIAG_SESSION_REFRESH_MS)
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

        case OBDCmd::READ_DID_BMW: {
            // [TARGET][DID_hi][DID_lo]
            // BMW F-Series Style: TX-ID 0x6F1, Daten = [TARGET][PCI=0x03][0x22][DID_hi][DID_lo]
            // ECU antwortet auf 0x600+TARGET mit [SOURCE=0xF1][PCI][0x62][DID_hi][DID_lo][data]
            if (len < 3) { ble.sendResponse(cmd, OBDStatus::ERR_NO_RESP); return; }
            uint8_t  target = data[0];
            uint16_t did    = ((uint16_t)data[1] << 8) | data[2];

            // CAN muss aktiv sein; Mode-22 braucht D-CAN/PT-CAN @ 500k.
            if (activeBus == BusType::NONE || activeBus == BusType::KLINE) {
                Serial.printf("[BMW] DID 0x%04X target=0x%02X — REJECTED (no CAN bus)\n",
                              did, target);
                ble.sendResponse(cmd, OBDStatus::ERR_NOT_INIT);
                break;
            }

            uint32_t txId  = BMW_TESTER_ADDR;          // 0x6F1
            uint32_t rxId  = 0x600 + target;           // 0x612 DDE/DME, 0x618 EGS, 0x629 DSC, ...

            // Erste BMW-Anfrage einer Session laut loggen — wenn diese Zeile
            // gar nicht erst kommt, schickt der Laptimer keine 0x16 Frames
            // und der Patch greift nicht.
            static bool s_bmw_first = true;
            if (s_bmw_first) {
                s_bmw_first = false;
                Serial.printf("[BMW] First Mode-22 request: DID 0x%04X "
                              "target=0x%02X (TX 0x%03X → RX 0x%03X)\n",
                              did, target, txId, rxId);
            }

            isotp.setBmwExtended(true, target);
            uint32_t t0 = millis();
            bool ok = uds.readDid(did, resp, txId, rxId);
            uint32_t dt = millis() - t0;
            isotp.setBmwExtended(false);               // immer zurückstellen!

            if (ok) {
                ble.sendResponse(cmd, OBDStatus::OK, resp.data, resp.len);
                // Nur erste erfolgreiche Antwort laut loggen pro Boot —
                // sonst spammt das bei 5 Hz Polling.
                static bool s_first_ok = true;
                if (s_first_ok) {
                    s_first_ok = false;
                    Serial.printf("[BMW] First successful DID response: "
                                  "0x%04X target=0x%02X, %d bytes in %ums\n",
                                  did, target, (int)resp.len, (unsigned)dt);
                }
            } else if (resp.negative) {
                // NRC = Negative Response Code. Häufige Codes:
                //   0x10 generalReject, 0x11 serviceNotSupported,
                //   0x12 subFunctionNotSupported, 0x22 conditionsNotCorrect,
                //   0x31 requestOutOfRange, 0x33 securityAccessDenied,
                //   0x7E subFunctionNotSupportedInActiveSession,
                //   0x7F serviceNotSupportedInActiveSession.
                Serial.printf("[BMW] NRC 0x%02X for DID 0x%04X target=0x%02X "
                              "(took %ums)\n",
                              (unsigned)resp.nrc, did, target, (unsigned)dt);
                ble.sendResponse(cmd, OBDStatus::ERR_NEGATIVE,
                                 &resp.nrc, 1);
            } else {
                // Timeout — DDE hat innerhalb UDS_RESP_TIMEOUT_MS nicht geantwortet.
                // In den ersten 1-2 Tries pro Session normal (DDE muss aufwachen).
                // Wenn das durchgehend kommt: TX-Frame geht raus aber kein RX
                // → entweder falscher target, falsche RX-ID, oder DDE schläft.
                Serial.printf("[BMW] Timeout DID 0x%04X target=0x%02X "
                              "(no response in %ums)\n",
                              did, target, (unsigned)dt);
                ble.sendResponse(cmd, OBDStatus::ERR_TIMEOUT);
            }
            break;
        }

        case OBDCmd::READ_DID_2C: {
            if (len < 2) { ble.sendResponse(cmd, OBDStatus::ERR_NO_RESP); return; }
            uint16_t did = ((uint16_t)data[0] << 8) | data[1];
            ensureDiagSession(BMW_TESTER_ADDR, BMW_DME_RESP);
            if (uds.readDid2C(did, resp, BMW_TESTER_ADDR, BMW_DME_RESP))
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

        case OBDCmd::SET_ECU: {
            if (len < 1 || data[0] >= BMW_ECU_COUNT) {
                ble.sendResponse(cmd, OBDStatus::ERR_NO_RESP); return;
            }
            activeTxId = BMW_ECUS[data[0]].txId;
            activeRxId = BMW_ECUS[data[0]].rxId;
            Serial.printf("[CMD] ECU → %s\n", BMW_ECUS[data[0]].name);
            ble.sendResponse(cmd, OBDStatus::OK);
            break;
        }

        case OBDCmd::SET_ECU_DIAG:
            activeTxId = BMW_TESTER_ADDR;
            activeRxId = BMW_DME_RESP;
            ble.sendResponse(cmd, OBDStatus::OK);
            break;

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
    Serial.println("  BRL OBD Adapter v1.0");
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
        activeTxId = OBD2_DME_REQ;
        activeRxId = OBD2_DME_RESP;
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

    // Auto-Init D-CAN @ 500k. Der BRL-Laptimer schickt kein SET_BUS_CAN —
    // ohne diesen Block würde Mode-22-BMW gar nicht erst rauskommen weil
    // der TWAI-Treiber nicht installiert ist. Mode-01 funktionierte bisher
    // nur, weil der ELM-Emulator-Pfad (NUS) intern canInit() angetriggert
    // hat. Für unseren Binary-Pfad setzen wir den Standard-Bus jetzt fix
    // beim Boot — D-CAN/PT-CAN @ 500 kBit/s ist der Bus auf dem 99% aller
    // BMW-Diagnose-Daten liegen (E-Series ab ca. 2007, alle F/G-Series).
    Serial.println("[BOOT] Auto-Init D-CAN @ 500k...");
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
