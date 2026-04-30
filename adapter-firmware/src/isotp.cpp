// =============================================================================
//  ISO-TP (ISO 15765-2) Transport Protocol – Vollständige Implementierung
//
//  Standard-Modus: PCI-Byte ist das erste Datenbyte des CAN-Frames.
//  BMW-Extended (F-Series Diag, 0x6F1 ↔ 0x612):
//    - TX: erstes Datenbyte = TARGET (z.B. 0x12 für DDE), dann PCI
//    - RX: erstes Datenbyte = SOURCE (immer 0xF1), wird vor dem PCI-Parse
//      verworfen
//    - SF nutzbar für 6 statt 7 Nutzbytes (1 Byte für Adressierung weg)
//    - FF nutzbar für 5 statt 6 Bytes
//    - CF nutzbar für 6 statt 7 Bytes
//    - FlowControl muss ebenfalls TARGET-prefix haben
// =============================================================================
#include "isotp.h"
#include "adapter_config.h"

void IsotpHandler::setBmwExtended(bool enabled, uint8_t targetByte) {
    _bmwExt    = enabled;
    _bmwTarget = targetByte;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send – Sendet Daten als ISO-TP (automatisch SF oder FF+CF)
// ─────────────────────────────────────────────────────────────────────────────
bool IsotpHandler::send(uint32_t txId, uint32_t rxId,
                        const uint8_t* data, uint16_t len) {
    if (len == 0 || len > ISOTP_MAX_LEN) return false;

    // Single-Frame-Grenze: 7 Bytes Standard, 6 Bytes mit BMW-Extended
    const uint16_t sfMaxLen = _bmwExt ? 6 : 7;
    if (len <= sfMaxLen) {
        return _sendSingleFrame(txId, data, len);
    }

    // Multi-Frame: First Frame senden, dann auf FC warten
    _txCanId   = txId;
    _fcRxId    = rxId;
    _txLen     = len;
    _txSent    = 0;
    _txSeq     = 1;
    _txWaitFC  = true;
    memcpy(_txBuf, data, len);

    if (!_sendFirstFrame(txId, data, len)) return false;

    // Warte auf Flow Control
    uint32_t start = millis();
    while (_txWaitFC && (millis() - start) < ISOTP_FC_TIMEOUT_MS) {
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(10)) == ESP_OK) {
            if (rx.identifier == rxId && rx.data_length_code >= 3) {
                // BMW-Extended: erstes Byte ist Source, dann erst PCI
                uint8_t off = _bmwExt ? 1 : 0;
                if (rx.data_length_code <= off) continue;
                uint8_t pci = rx.data[off] & 0xF0;
                if (pci == ISOTP_FC) {
                    uint8_t fcStatus = rx.data[off] & 0x0F;
                    if (fcStatus == FC_ABORT) return false;
                    if (fcStatus == FC_CTS) {
                        _fcBlockSize = rx.data[off + 1];
                        _fcStMin     = rx.data[off + 2];
                        _fcBlockCnt  = 0;
                        _txWaitFC    = false;
                    }
                    // FC_WAIT: weiter warten
                }
            }
        }
    }
    if (_txWaitFC) return false;  // FC Timeout

    // Consecutive Frames senden
    return _sendConsecutiveFrames(txId);
}

// ─────────────────────────────────────────────────────────────────────────────
//  receive – Empfängt CAN-Frame, baut ISO-TP Nachricht zusammen
// ─────────────────────────────────────────────────────────────────────────────
bool IsotpHandler::receive(const twai_message_t& frame, IsotpMessage& msg) {
    if (frame.data_length_code < 1) return false;

    // BMW-Extended: erstes Byte = Source-ID (z.B. 0xF1 vom DDE), wird übersprungen
    const uint8_t pciOff = _bmwExt ? 1 : 0;
    if (frame.data_length_code <= pciOff) return false;

    uint8_t pci = frame.data[pciOff] & 0xF0;
    msg.canId   = frame.identifier;

    switch (pci) {
        case ISOTP_SF: {
            // Single Frame: Byte pciOff = 0x0L (L = Datenlänge, 1-7)
            uint8_t len = frame.data[pciOff] & 0x0F;
            if (len == 0 || len > 7
                || len > frame.data_length_code - pciOff - 1)
                return false;
            memcpy(msg.data, frame.data + pciOff + 1, len);
            msg.len      = len;
            msg.complete = true;
            return true;
        }

        case ISOTP_FF: {
            // First Frame: bytes pciOff..pciOff+1 = 0x1LLL (L = Gesamtlänge)
            if (frame.data_length_code < pciOff + 2) return false;
            uint16_t totalLen =
                ((uint16_t)(frame.data[pciOff] & 0x0F) << 8)
                | frame.data[pciOff + 1];
            if (totalLen < 8 || totalLen > sizeof(_rxBuf)) {
                return false;
            }
            _rxExpected = totalLen;
            _rxGot      = 0;
            _rxSeq      = 1;
            _rxCanId    = frame.identifier;
            _rxActive   = true;
            _lastRxTime = millis();

            // Erste Daten kopieren — Standard: 6 Bytes, BMW-Extended: 5 Bytes
            uint8_t firstFrameDataMax = _bmwExt ? 5 : 6;
            uint8_t firstLen =
                (totalLen > firstFrameDataMax) ? firstFrameDataMax : totalLen;
            memcpy(_rxBuf, frame.data + pciOff + 2, firstLen);
            _rxGot = firstLen;

            // Flow Control senden — Empfangs-ID → Sende-ID
            uint32_t fcId;
            if (frame.identifier >= 0x7E8 && frame.identifier <= 0x7EF) {
                fcId = frame.identifier - 8;  // 7E8→7E0, 7E9→7E1
            } else if (_bmwExt || frame.identifier == BMW_DME_RESP) {
                fcId = BMW_TESTER_ADDR;        // 612→6F1
            } else {
                fcId = frame.identifier - 8;   // Generisch
            }
            sendFlowControl(fcId, FC_CTS, 0, 0);
            return false;  // Noch nicht komplett
        }

        case ISOTP_CF: {
            // Consecutive Frame: Byte pciOff = 0x2N (N = Sequenznummer 0-F)
            if (!_rxActive || frame.identifier != _rxCanId) return false;

            uint8_t seq = frame.data[pciOff] & 0x0F;
            if (seq != (_rxSeq & 0x0F)) {
                Serial.printf("[ISOTP] CF seq mismatch: got %d, expected %d\n",
                              seq, _rxSeq & 0x0F);
                _rxActive = false;
                return false;
            }
            _rxSeq++;
            _lastRxTime = millis();

            uint16_t remaining = _rxExpected - _rxGot;
            uint8_t  cfDataMax = _bmwExt ? 6 : 7;
            uint8_t  copyLen   =
                (remaining > cfDataMax) ? cfDataMax : remaining;
            uint8_t  available =
                (frame.data_length_code > pciOff + 1)
                    ? (frame.data_length_code - pciOff - 1) : 0;
            if (copyLen > available) copyLen = available;
            memcpy(_rxBuf + _rxGot, frame.data + pciOff + 1, copyLen);
            _rxGot += copyLen;

            if (_rxGot >= _rxExpected) {
                // Komplett!
                memcpy(msg.data, _rxBuf, _rxGot);
                msg.len      = _rxGot;
                msg.complete = true;
                _rxActive    = false;
                return true;
            }
            return false;  // Weitere CFs erwartet
        }

        case ISOTP_FC:
            // Flow Control empfangen (während wir senden) – hier nicht behandelt,
            // wird in send() inline verarbeitet
            return false;

        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void IsotpHandler::sendFlowControl(uint32_t txId, uint8_t status,
                                    uint8_t blockSize, uint8_t stMin) {
    uint8_t data[8] = {};
    if (_bmwExt) {
        data[0] = _bmwTarget;
        data[1] = ISOTP_FC | (status & 0x0F);
        data[2] = blockSize;
        data[3] = stMin;
    } else {
        data[0] = ISOTP_FC | (status & 0x0F);
        data[1] = blockSize;
        data[2] = stMin;
    }
    _txCanFrame(txId, data, 8);
}

void IsotpHandler::reset() {
    _rxActive  = false;
    _rxGot     = 0;
    _txWaitFC  = false;
    _txSent    = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Interne Sende-Funktionen
// ─────────────────────────────────────────────────────────────────────────────
bool IsotpHandler::_sendSingleFrame(uint32_t txId, const uint8_t* data, uint16_t len) {
    uint8_t frame[8] = {};
    if (_bmwExt) {
        frame[0] = _bmwTarget;
        frame[1] = ISOTP_SF | (len & 0x0F);
        memcpy(frame + 2, data, len);
    } else {
        frame[0] = ISOTP_SF | (len & 0x0F);
        memcpy(frame + 1, data, len);
    }
    return _txCanFrame(txId, frame, 8);
}

bool IsotpHandler::_sendFirstFrame(uint32_t txId, const uint8_t* data, uint16_t len) {
    uint8_t frame[8] = {};
    uint8_t firstChunk;
    if (_bmwExt) {
        frame[0] = _bmwTarget;
        frame[1] = ISOTP_FF | ((len >> 8) & 0x0F);
        frame[2] = len & 0xFF;
        firstChunk = 5;  // 8 - 1 (target) - 2 (FF header) = 5
        memcpy(frame + 3, data, firstChunk);
    } else {
        frame[0] = ISOTP_FF | ((len >> 8) & 0x0F);
        frame[1] = len & 0xFF;
        firstChunk = 6;
        memcpy(frame + 2, data, firstChunk);
    }
    _txSent = firstChunk;
    return _txCanFrame(txId, frame, 8);
}

bool IsotpHandler::_sendConsecutiveFrames(uint32_t txId) {
    while (_txSent < _txLen) {
        uint8_t frame[8] = {};
        uint8_t cfDataMax;
        if (_bmwExt) {
            frame[0] = _bmwTarget;
            frame[1] = ISOTP_CF | (_txSeq & 0x0F);
            cfDataMax = 6;
        } else {
            frame[0] = ISOTP_CF | (_txSeq & 0x0F);
            cfDataMax = 7;
        }
        _txSeq++;

        uint16_t remaining = _txLen - _txSent;
        uint8_t copyLen = (remaining > cfDataMax) ? cfDataMax : remaining;
        memcpy(frame + (_bmwExt ? 2 : 1), _txBuf + _txSent, copyLen);
        _txSent += copyLen;

        if (!_txCanFrame(txId, frame, 8)) return false;

        // STmin Delay
        if (_fcStMin > 0 && _fcStMin <= 127) {
            delay(_fcStMin);
        } else if (_fcStMin >= 0xF1 && _fcStMin <= 0xF9) {
            delayMicroseconds((_fcStMin - 0xF0) * 100);
        }

        // Block Size Check: nach N Frames auf neuen FC warten
        if (_fcBlockSize > 0) {
            _fcBlockCnt++;
            if (_fcBlockCnt >= _fcBlockSize && _txSent < _txLen) {
                _fcBlockCnt = 0;
                _txWaitFC   = true;
                uint32_t start = millis();
                while (_txWaitFC && (millis() - start) < ISOTP_FC_TIMEOUT_MS) {
                    twai_message_t rx;
                    if (twai_receive(&rx, pdMS_TO_TICKS(10)) == ESP_OK) {
                        if (rx.identifier == _fcRxId) {
                            uint8_t off = _bmwExt ? 1 : 0;
                            if (rx.data_length_code <= off) continue;
                            if ((rx.data[off] & 0xF0) == ISOTP_FC) {
                                uint8_t st = rx.data[off] & 0x0F;
                                if (st == FC_CTS) { _txWaitFC = false; }
                                else if (st == FC_ABORT) return false;
                            }
                        }
                    }
                }
                if (_txWaitFC) return false;
            }
        }
    }
    return true;
}

bool IsotpHandler::_txCanFrame(uint32_t canId, const uint8_t* data, uint8_t dlc) {
    twai_message_t msg = {};
    msg.identifier       = canId;
    msg.data_length_code = dlc;
    memcpy(msg.data, data, dlc);
    return twai_transmit(&msg, pdMS_TO_TICKS(50)) == ESP_OK;
}
