// =============================================================================
//  K-Line Handler – ISO 9141-2 / KWP2000 für BMW E-Serie
// =============================================================================
#include "kline.h"
#include "adapter_config.h"

#define KWP_TESTER_ADDR  0xF1   // Externer Tester
#define KWP_HEADER_FMT   0x80   // Format-Byte: Physische Adressierung + Länge

bool KLineHandler::begin(uint8_t txPin, uint8_t rxPin) {
    _serial = &Serial2;
    _serial->begin(KLINE_BAUD, SERIAL_8N1, rxPin, txPin);
    _connected = false;
    Serial.printf("[K-Line] UART2 init: TX=%d RX=%d %d Baud\n",
                  txPin, rxPin, KLINE_BAUD);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ISO 9141-2 Slow Init (5-Baud)
//  Sendet Adresse bei 5 Baud, wartet auf Sync-Bytes (0x55, KB1, KB2)
// ─────────────────────────────────────────────────────────────────────────────
bool KLineHandler::slowInit(uint8_t targetAddr) {
    Serial.printf("[K-Line] Slow Init (5-Baud) Addr=0x%02X\n", targetAddr);

    // UART kurz stoppen, TX auf LOW für > 300ms
    _serial->end();
    pinMode(PIN_KLINE_TX, OUTPUT);

    // 5-Baud Init: Adresse bitweise senden (200ms pro Bit)
    _send5Baud(targetAddr);

    // UART wieder mit 10400 Baud starten
    _serial->begin(KLINE_BAUD, SERIAL_8N1, PIN_KLINE_RX, PIN_KLINE_TX);
    _serial->flush();

    // Auf Sync-Byte 0x55 warten (max 300ms)
    uint32_t start = millis();
    while ((millis() - start) < 300) {
        if (_serial->available()) {
            uint8_t sync = _serial->read();
            if (sync == 0x55) {
                Serial.println("[K-Line] Sync 0x55 empfangen");

                // Key Bytes lesen (KB1, KB2)
                uint8_t kb1 = 0, kb2 = 0;
                if (_serial->available()) kb1 = _serial->read();
                delay(5);
                if (_serial->available()) kb2 = _serial->read();

                Serial.printf("[K-Line] KeyBytes: %02X %02X\n", kb1, kb2);

                // Invertiertes KB2 als Bestätigung senden
                delay(25);
                _serial->write(~kb2);

                // Auf invertierte Adresse vom ECU warten
                delay(50);
                if (_serial->available()) {
                    uint8_t invAddr = _serial->read();
                    Serial.printf("[K-Line] ECU Bestätigung: 0x%02X\n", invAddr);
                    _connected = true;
                    _lastComm  = millis();
                    return true;
                }
            }
        }
        delay(1);
    }

    Serial.println("[K-Line] Slow Init fehlgeschlagen");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  KWP2000 Fast Init (ISO 14230)
//  25ms LOW, 25ms HIGH, dann StartCommunication Request
// ─────────────────────────────────────────────────────────────────────────────
bool KLineHandler::fastInit() {
    Serial.println("[K-Line] Fast Init (KWP2000)");

    _serial->end();
    pinMode(PIN_KLINE_TX, OUTPUT);

    // Wakeup Pattern: 25ms LOW → 25ms HIGH
    digitalWrite(PIN_KLINE_TX, LOW);
    delay(25);
    digitalWrite(PIN_KLINE_TX, HIGH);
    delay(25);

    // UART starten
    _serial->begin(KLINE_BAUD, SERIAL_8N1, PIN_KLINE_RX, PIN_KLINE_TX);

    // StartCommunication Request (Service 0x81)
    uint8_t startComm[] = { 0x81 };
    _sendKwpFrame(0x33, KWP_TESTER_ADDR, startComm, 1);

    // Antwort: C1 + KeyBytes
    uint8_t resp[16];
    uint8_t respLen = 0;
    if (_recvKwpFrame(resp, &respLen, sizeof(resp), 300)) {
        if (resp[0] == 0xC1) {
            Serial.printf("[K-Line] Fast Init OK, KeyBytes: %02X %02X\n",
                          resp[1], resp[2]);
            _connected = true;
            _lastComm  = millis();
            return true;
        }
    }

    Serial.println("[K-Line] Fast Init fehlgeschlagen");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  KWP2000 Request/Response
// ─────────────────────────────────────────────────────────────────────────────
bool KLineHandler::sendRequest(uint8_t targetAddr, const uint8_t* data, uint8_t len,
                                uint8_t* response, uint8_t* respLen, uint8_t maxRespLen,
                                uint32_t timeoutMs) {
    if (!_connected) return false;

    _sendKwpFrame(targetAddr, KWP_TESTER_ADDR, data, len);
    _lastComm = millis();

    return _recvKwpFrame(response, respLen, maxRespLen, timeoutMs);
}

bool KLineHandler::readPidKLine(uint8_t pid, uint8_t* data, uint8_t* dataLen) {
    uint8_t req[2] = { 0x01, pid };  // OBD2 Mode 0x01
    uint8_t resp[32];
    uint8_t respLen = 0;

    if (!sendRequest(0x33, req, 2, resp, &respLen, sizeof(resp))) return false;

    // Response: 41 PID data...
    if (respLen >= 2 && resp[0] == 0x41 && resp[1] == pid) {
        uint8_t payloadLen = respLen - 2;
        memcpy(data, resp + 2, payloadLen);
        *dataLen = payloadLen;
        return true;
    }
    return false;
}

void KLineHandler::keepAlive() {
    if (!_connected) return;
    if ((millis() - _lastComm) < 2000) return;  // Alle 2s

    uint8_t testerPresent[] = { 0x3E };
    _sendKwpFrame(0x33, KWP_TESTER_ADDR, testerPresent, 1);
    _lastComm = millis();

    // Response lesen und verwerfen
    uint8_t buf[8];
    uint8_t len = 0;
    _recvKwpFrame(buf, &len, sizeof(buf), 200);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Interne Funktionen
// ─────────────────────────────────────────────────────────────────────────────
void KLineHandler::_send5Baud(uint8_t byte) {
    // Start-Bit (LOW, 200ms)
    digitalWrite(PIN_KLINE_TX, LOW);
    delay(200);

    // 8 Datenbits (LSB first, 200ms pro Bit)
    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(PIN_KLINE_TX, (byte >> i) & 1 ? HIGH : LOW);
        delay(200);
    }

    // Stop-Bit (HIGH, 200ms)
    digitalWrite(PIN_KLINE_TX, HIGH);
    delay(200);
}

void KLineHandler::_sendKwpFrame(uint8_t target, uint8_t source,
                                  const uint8_t* data, uint8_t len) {
    uint8_t frame[64];
    uint8_t idx = 0;

    frame[idx++] = KWP_HEADER_FMT | len;  // Format + Länge
    frame[idx++] = target;
    frame[idx++] = source;
    memcpy(frame + idx, data, len);
    idx += len;
    frame[idx] = _checksum(frame, idx);
    idx++;

    // Inter-byte delay: 5ms zwischen Bytes (P4 timing)
    for (uint8_t i = 0; i < idx; i++) {
        _serial->write(frame[i]);
        if (i < idx - 1) delayMicroseconds(5000);
    }
    _serial->flush();
}

bool KLineHandler::_recvKwpFrame(uint8_t* data, uint8_t* len, uint8_t maxLen,
                                  uint32_t timeoutMs) {
    uint32_t start = millis();
    uint8_t  frame[64];
    uint8_t  idx = 0;

    // Format-Byte lesen
    while ((millis() - start) < timeoutMs) {
        if (_serial->available()) {
            frame[idx++] = _serial->read();
            break;
        }
        delay(1);
    }
    if (idx == 0) return false;

    uint8_t payloadLen = frame[0] & 0x3F;
    uint8_t headerLen  = (frame[0] & 0x80) ? 3 : 1;  // Physisch: 3, Funktional: 1
    uint8_t totalExpected = headerLen + payloadLen + 1;  // +1 für Checksum

    // Rest lesen
    while (idx < totalExpected && (millis() - start) < timeoutMs) {
        if (_serial->available()) {
            frame[idx++] = _serial->read();
        } else {
            delay(1);
        }
    }

    if (idx < totalExpected) return false;

    // Checksum prüfen
    uint8_t cs = _checksum(frame, idx - 1);
    if (cs != frame[idx - 1]) {
        Serial.printf("[K-Line] Checksum Fehler: got 0x%02X, expected 0x%02X\n",
                      frame[idx - 1], cs);
        return false;
    }

    // Payload extrahieren (ohne Header + Checksum)
    uint8_t copyLen = (payloadLen > maxLen) ? maxLen : payloadLen;
    memcpy(data, frame + headerLen, copyLen);
    *len = copyLen;
    return true;
}

uint8_t KLineHandler::_checksum(const uint8_t* data, uint8_t len) {
    uint8_t sum = 0;
    for (uint8_t i = 0; i < len; i++) sum += data[i];
    return sum;
}
