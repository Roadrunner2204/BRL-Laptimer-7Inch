// =============================================================================
//  UDS Client + OBD2 – Universeller BMW Diagnose-Client
// =============================================================================
#include "uds_client.h"
#include "adapter_config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Interne Sende/Empfangs-Logik
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::_sendAndReceive(const uint8_t* req, uint16_t reqLen,
                                 UdsResponse& resp,
                                 uint32_t txId, uint32_t rxId,
                                 uint32_t timeoutMs) {
    resp.len      = 0;
    resp.negative = false;
    resp.nrc      = 0;

    _isotp.reset();

    if (!_isotp.send(txId, rxId, req, reqLen)) {
        Serial.printf("[UDS] Send failed (0x%03X)\n", txId);
        return false;
    }

    // Antwort empfangen (mit ISO-TP Reassembly)
    IsotpMessage isoMsg = {};
    uint32_t start = millis();

    while ((millis() - start) < timeoutMs) {
        twai_message_t frame;
        if (twai_receive(&frame, pdMS_TO_TICKS(10)) == ESP_OK) {
            if (frame.identifier == rxId) {
                if (_isotp.receive(frame, isoMsg)) {
                    // Komplett empfangen
                    memcpy(resp.data, isoMsg.data, isoMsg.len);
                    resp.len       = isoMsg.len;
                    resp.serviceId = isoMsg.data[0];

                    // Negative Response prüfen
                    if (resp.serviceId == 0x7F && resp.len >= 3) {
                        resp.negative = true;
                        resp.nrc      = resp.data[2];
                    }
                    return !resp.negative;
                }
            }
        }
    }

    Serial.printf("[UDS] Timeout waiting for 0x%03X\n", rxId);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD2 Mode 0x01 – PID lesen
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::readPid(uint8_t pid, UdsResponse& resp,
                         uint32_t txId, uint32_t rxId) {
    uint8_t req[2] = { OBD2_CURRENT_DATA, pid };
    // ECU antwortet typisch in 5-50ms. 80ms Timeout = großzügig genug für
    // langsame ECUs, aber dead-PID-Recovery ist schnell statt 150ms blocked.
    return _sendAndReceive(req, 2, resp, txId, rxId, 80);
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD2 Mode 0x01 – Multi-PID (bis zu 6 PIDs in einem Request)
//  CAN-Frame: [01] [PID1] [PID2] ... [PIDn]
//  Antwort:   [41] [PID1] [data...] [PID2] [data...] ...
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::readMultiPid(const uint8_t* pids, uint8_t count,
                              UdsResponse& resp,
                              uint32_t txId, uint32_t rxId) {
    if (count < 1 || count > 6) return false;
    uint8_t req[7];  // 1 Service + max 6 PIDs
    req[0] = OBD2_CURRENT_DATA;
    memcpy(req + 1, pids, count);
    // Multi-PID: ECU braucht etwas länger weil bis zu 6 PIDs in einem
    // Request, plus die Antwort ist ISO-TP-Multi-Frame (FF + CFs + FC).
    // 100ms ist immer noch defensiv — typisch ~20-60ms Antwort.
    return _sendAndReceive(req, 1 + count, resp, txId, rxId, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD2 Mode 0x09 PID 0x02 – VIN lesen
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::readVin(char* vinBuf, uint8_t maxLen) {
    uint8_t req[2] = { OBD2_VEHICLE_INFO, 0x02 };
    UdsResponse resp;

    if (!_sendAndReceive(req, 2, resp, OBD2_DME_REQ, OBD2_DME_RESP, 800))
        return false;

    // Response: 49 02 01 <17 VIN bytes>
    if (resp.len < 5 || resp.data[0] != 0x49) return false;

    uint8_t vinStart = (resp.data[1] == 0x02 && resp.data[2] == 0x01) ? 3 : 2;
    uint8_t vinLen   = resp.len - vinStart;
    if (vinLen > 17) vinLen = 17;
    if (vinLen > maxLen - 1) vinLen = maxLen - 1;

    memcpy(vinBuf, resp.data + vinStart, vinLen);
    vinBuf[vinLen] = '\0';
    return vinLen > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD2 Mode 0x03 – DTCs lesen
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::readDtcsObd2(UdsResponse& resp) {
    uint8_t req[1] = { OBD2_READ_DTC };
    return _sendAndReceive(req, 1, resp, OBD2_DME_REQ, OBD2_DME_RESP, 2000);
}

bool UdsClient::clearDtcsObd2() {
    uint8_t req[1] = { OBD2_CLEAR_DTC };
    UdsResponse resp;
    return _sendAndReceive(req, 1, resp, OBD2_DME_REQ, OBD2_DME_RESP, 3000);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UDS Mode 0x22 – ReadDataByIdentifier (B48/B58/MG1)
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::readDid(uint16_t did, UdsResponse& resp,
                         uint32_t txId, uint32_t rxId) {
    uint8_t req[3] = {
        UDS_READ_DID,
        (uint8_t)(did >> 8),
        (uint8_t)(did & 0xFF)
    };
    return _sendAndReceive(req, 3, resp, txId, rxId, 250);  // UDS Single-Frame: ~10-100ms
}

// ─────────────────────────────────────────────────────────────────────────────
//  UDS Mode 0x2C – DynamicallyDefineDataIdentifier (N47 DDE7.1)
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::readDid2C(uint16_t did, UdsResponse& resp,
                           uint32_t txId, uint32_t rxId) {
    uint8_t req[4] = {
        UDS_DYN_DEF_DID,
        0x10,
        (uint8_t)(did >> 8),
        (uint8_t)(did & 0xFF)
    };
    return _sendAndReceive(req, 4, resp, txId, rxId, 250);  // UDS Single-Frame
}

// ─────────────────────────────────────────────────────────────────────────────
//  UDS Mode 0x19 – ReadDTCInformation
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::readDtcsUds(UdsResponse& resp,
                             uint32_t txId, uint32_t rxId) {
    // Subfunction 0x02 = reportDTCByStatusMask, Mask 0xAF
    uint8_t req[3] = { UDS_READ_DTC_INFO, 0x02, 0xAF };
    return _sendAndReceive(req, 3, resp, txId, rxId, 3000);
}

bool UdsClient::clearDtcsUds(uint32_t txId, uint32_t rxId) {
    // Clear all DTC groups (0xFFFFFF)
    uint8_t req[4] = { UDS_CLEAR_DTC, 0xFF, 0xFF, 0xFF };
    UdsResponse resp;
    return _sendAndReceive(req, 4, resp, txId, rxId, 5000);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UDS Mode 0x10 – DiagnosticSessionControl
// ─────────────────────────────────────────────────────────────────────────────
bool UdsClient::startDiagSession(uint8_t sessionType,
                                  uint32_t txId, uint32_t rxId) {
    uint8_t req[2] = { UDS_DIAG_SESSION, sessionType };
    UdsResponse resp;
    return _sendAndReceive(req, 2, resp, txId, rxId, 500);  // ECU antwortet in ~10-50ms
}

bool UdsClient::testerPresent(uint32_t txId, uint32_t rxId) {
    uint8_t req[2] = { UDS_TESTER_PRESENT, 0x00 };
    UdsResponse resp;
    return _sendAndReceive(req, 2, resp, txId, rxId, 200);
}
