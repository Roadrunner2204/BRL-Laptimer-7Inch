// =============================================================================
//  ELM327/ELM329 Emulator – ASCII AT-Commands über BLE UART
// =============================================================================
#include "elm_emulator.h"
#include "adapter_config.h"
#include <ctype.h>
#include <string.h>

ElmEmulator::ElmEmulator(UdsClient& uds, IsotpHandler& isotp)
    : _uds(uds), _isotp(isotp), _linePos(0) {
    memset(_lineBuf, 0, sizeof(_lineBuf));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Daten vom NUS RX empfangen (können Teilzeilen sein)
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::feed(const uint8_t* data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            if (_linePos > 0) {
                _lineBuf[_linePos] = '\0';
                if (_cfg.echo) {
                    _send(_lineBuf);
                    _send("\r");
                }
                _processLine(_lineBuf);
                _linePos = 0;
            }
        } else if (_linePos < sizeof(_lineBuf) - 1) {
            _lineBuf[_linePos++] = c;
        }
    }
}

void ElmEmulator::update() {
    // Für Monitor-Mode (Zukunft)
}

// ─────────────────────────────────────────────────────────────────────────────
//  Zeile verarbeiten
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_processLine(const char* line) {
    // Whitespace trimmen
    while (*line == ' ') line++;
    if (*line == '\0') { _sendPrompt(); return; }

    // AT-Befehl?
    if ((line[0] == 'A' || line[0] == 'a') && (line[1] == 'T' || line[1] == 't')) {
        _handleAT(line + 2);
    } else {
        _handleOBD(line);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  AT-Befehle
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_handleAT(const char* cmd) {
    // Whitespace überspringen
    while (*cmd == ' ') cmd++;

    // Uppercase-Kopie für Vergleich
    char uc[32] = {};
    for (int i = 0; i < 31 && cmd[i]; i++) uc[i] = toupper(cmd[i]);

    // ── Reset / Info ──────────────────────────────────────────────
    if (uc[0] == 'Z') {
        _cfg = ElmConfig();
        _sendLine("");
        _sendLine(ELM_VERSION);
        _sendPrompt();
        return;
    }
    if (uc[0] == 'I') { _sendLine(ELM_VERSION); _sendPrompt(); return; }
    if (uc[0] == '@' && uc[1] == '1') { _sendLine(ELM_DEVICE); _sendPrompt(); return; }
    if (uc[0] == '@' && uc[1] == '2') { _sendLine(ELM_DEVICE); _sendPrompt(); return; }

    // ── Echo ──────────────────────────────────────────────────────
    if (uc[0] == 'E' && uc[1] == '0') { _cfg.echo = false; _sendOK(); return; }
    if (uc[0] == 'E' && uc[1] == '1') { _cfg.echo = true;  _sendOK(); return; }

    // ── Linefeed ──────────────────────────────────────────────────
    if (uc[0] == 'L' && uc[1] == '0') { _cfg.linefeed = false; _sendOK(); return; }
    if (uc[0] == 'L' && uc[1] == '1') { _cfg.linefeed = true;  _sendOK(); return; }

    // ── Spaces ────────────────────────────────────────────────────
    if (uc[0] == 'S' && uc[1] == '0') { _cfg.spaces = false; _sendOK(); return; }
    if (uc[0] == 'S' && uc[1] == '1') { _cfg.spaces = true;  _sendOK(); return; }

    // ── Headers ───────────────────────────────────────────────────
    if (uc[0] == 'H' && uc[1] == '0') { _cfg.headers = false; _sendOK(); return; }
    if (uc[0] == 'H' && uc[1] == '1') { _cfg.headers = true;  _sendOK(); return; }

    // ── Set Protocol ──────────────────────────────────────────────
    if (uc[0] == 'S' && uc[1] == 'P') {
        char pChar = uc[2] == ' ' ? uc[3] : uc[2];
        uint8_t proto = _hexNibble(pChar);
        _cfg.protocol = (ElmProtocol)proto;
        if (_busInitCb) {
            if (proto == 6 || proto == 7)
                _busInitCb(CAN_BAUD_DCAN);
            else if (proto == 8 || proto == 9)
                _busInitCb(CAN_BAUD_KCAN);
        }
        _sendOK();
        return;
    }

    // ── Set Header ────────────────────────────────────────────────
    if (uc[0] == 'S' && uc[1] == 'H') {
        const char* hex = cmd + 2;
        while (*hex == ' ') hex++;
        uint32_t hdr = 0;
        for (int i = 0; hex[i] && i < 8; i++) {
            hdr = (hdr << 4) | _hexNibble(hex[i]);
        }
        _cfg.canHeader = hdr;
        _sendOK();
        return;
    }

    // ── Set Timeout ───────────────────────────────────────────────
    if (uc[0] == 'S' && uc[1] == 'T') {
        const char* hex = cmd + 2;
        while (*hex == ' ') hex++;
        _cfg.timeout = (_hexNibble(hex[0]) << 4) | _hexNibble(hex[1]);
        _sendOK();
        return;
    }

    // ── Defaults ──────────────────────────────────────────────────
    if (uc[0] == 'D' && (uc[1] == '\0' || uc[1] == ' ')) {
        _cfg = ElmConfig();
        _sendOK();
        return;
    }

    // ── Describe Protocol ─────────────────────────────────────────
    if (uc[0] == 'D' && uc[1] == 'P') {
        if (uc[2] == 'N') {
            // AT DPN: protocol number
            char buf[4]; snprintf(buf, 4, "A%d", (int)_cfg.protocol);
            _sendLine(buf);
        } else {
            _sendLine("ISO 15765-4 (CAN 11/500)");
        }
        _sendPrompt();
        return;
    }

    // ── Read Voltage ──────────────────────────────────────────────
    if (uc[0] == 'R' && uc[1] == 'V') {
        _sendLine("12.4V");
        _sendPrompt();
        return;
    }

    // ── ELM329: CAN Flow Control ─────────────────────────────────
    if (uc[0] == 'C' && uc[1] == 'F' && uc[2] == 'C') {
        _cfg.canFlowCtrl = (uc[3] == '1');
        _sendOK();
        return;
    }

    // ── ELM329: CAN Auto Formatting ──────────────────────────────
    if (uc[0] == 'C' && uc[1] == 'A' && uc[2] == 'F') {
        _cfg.canAutoFmt = (uc[3] == '1');
        _sendOK();
        return;
    }

    // ── Adaptive Timing ──────────────────────────────────────────
    if (uc[0] == 'A' && uc[1] == 'T') {
        _sendOK(); return;  // AT AT0/AT1/AT2 – akzeptieren, ignorieren
    }

    // ── Warm Start ────────────────────────────────────────────────
    if (uc[0] == 'W' && uc[1] == 'S') {
        _cfg = ElmConfig();
        _sendLine(ELM_VERSION);
        _sendPrompt();
        return;
    }

    // ── Protocol Close ────────────────────────────────────────────
    if (uc[0] == 'P' && uc[1] == 'C') { _sendOK(); return; }

    // ── Memory Off ────────────────────────────────────────────────
    if (uc[0] == 'M' && uc[1] == '0') { _sendOK(); return; }

    // Unbekannt → OK (viele Apps senden optionale AT-Befehle)
    _sendOK();
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD Hex-String verarbeiten
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_handleOBD(const char* hex) {
    uint8_t data[16];
    uint16_t len = _hexToBytes(hex, data, sizeof(data));
    if (len < 1) { _sendError(); return; }

    uint8_t mode = data[0];
    switch (mode) {
        case 0x01: _execMode01(data + 1, len - 1); break;
        case 0x02: _execMode01(data + 1, len - 1); break;  // Freeze = gleich wie Mode 01
        case 0x03: _execMode03(); break;
        case 0x04: _execMode04(); break;
        case 0x09: _execMode09(len > 1 ? data[1] : 0x00); break;
        default:   _execRawUDS(data, len); break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD Mode 01 – Current Data
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_execMode01(const uint8_t* pids, uint8_t count) {
    if (count < 1) { _sendNoData(); return; }

    uint32_t txId = _cfg.canHeader;
    uint32_t rxId = _rxFromTx(txId);
    uint32_t timeoutMs = (uint32_t)_cfg.timeout * 4;
    if (timeoutMs < 50) timeoutMs = 200;

    for (uint8_t i = 0; i < count; i++) {
        UdsResponse resp;
        if (_uds.readPid(pids[i], resp, txId, rxId)) {
            _formatHexResponse(resp.data, resp.len, rxId);
        } else {
            _sendNoData();
            return;
        }
    }
    _sendPrompt();
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD Mode 03 – Read DTCs
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_execMode03() {
    UdsResponse resp;
    if (_uds.readDtcsObd2(resp)) {
        _formatHexResponse(resp.data, resp.len, _rxFromTx(_cfg.canHeader));
    } else {
        _sendNoData();
        return;
    }
    _sendPrompt();
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD Mode 04 – Clear DTCs
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_execMode04() {
    if (_uds.clearDtcsObd2()) {
        _sendLine("44");
    } else {
        _sendNoData();
        return;
    }
    _sendPrompt();
}

// ─────────────────────────────────────────────────────────────────────────────
//  OBD Mode 09 – Vehicle Info
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_execMode09(uint8_t pid) {
    if (pid == 0x02) {
        // VIN
        char vin[18] = {};
        if (_uds.readVin(vin, sizeof(vin))) {
            // Format: "49 02 01 <VIN als Hex>"
            char buf[128] = "49 02 01 ";
            int pos = 9;
            for (int i = 0; vin[i] && pos < 120; i++) {
                if (_cfg.spaces && i > 0) buf[pos++] = ' ';
                pos += snprintf(buf + pos, 4, "%02X", (uint8_t)vin[i]);
            }
            _sendLine(buf);
        } else {
            _sendNoData();
            return;
        }
    } else {
        _sendNoData();
        return;
    }
    _sendPrompt();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Raw UDS (Mode 0x22, 0x19, etc.)
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_execRawUDS(const uint8_t* data, uint8_t len) {
    uint32_t txId = _cfg.canHeader;
    uint32_t rxId = _rxFromTx(txId);

    _isotp.reset();
    if (!_isotp.send(txId, rxId, data, len)) {
        _sendNoData();
        return;
    }

    IsotpMessage isoMsg = {};
    uint32_t start = millis();
    uint32_t timeoutMs = (uint32_t)_cfg.timeout * 4;
    if (timeoutMs < 50) timeoutMs = 500;

    while ((millis() - start) < timeoutMs) {
        twai_message_t frame;
        if (twai_receive(&frame, pdMS_TO_TICKS(10)) == ESP_OK) {
            if (frame.identifier == rxId) {
                if (_isotp.receive(frame, isoMsg)) {
                    _formatHexResponse(isoMsg.data, isoMsg.len, rxId);
                    _sendPrompt();
                    return;
                }
            }
        }
    }
    _sendNoData();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Response-Formatierung (ELM327-Style)
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_formatHexResponse(const uint8_t* data, uint16_t len, uint32_t rxId) {
    char buf[512];
    int pos = 0;

    // Header (optional)
    if (_cfg.headers) {
        pos += snprintf(buf + pos, 16, "%03X ", (unsigned int)rxId);
        if (_cfg.spaces) {
            pos += snprintf(buf + pos, 8, "%02X ", len);
        } else {
            pos += snprintf(buf + pos, 8, "%02X", len);
        }
    }

    // Datenbytes
    for (uint16_t i = 0; i < len && pos < 500; i++) {
        if (i > 0 && _cfg.spaces) buf[pos++] = ' ';
        pos += snprintf(buf + pos, 4, "%02X", data[i]);
    }
    buf[pos] = '\0';
    _sendLine(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sende-Hilfsfunktionen
// ─────────────────────────────────────────────────────────────────────────────
void ElmEmulator::_send(const char* str) {
    if (_sendCb && str) _sendCb(str, strlen(str));
}

void ElmEmulator::_sendLine(const char* str) {
    _send(str);
    _send("\r");
    if (_cfg.linefeed) _send("\n");
}

void ElmEmulator::_sendOK() { _sendLine("OK"); _sendPrompt(); }
void ElmEmulator::_sendError() { _sendLine("?"); _sendPrompt(); }
void ElmEmulator::_sendNoData() { _sendLine("NO DATA"); _sendPrompt(); }
void ElmEmulator::_sendPrompt() { _send(">"); }

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
uint32_t ElmEmulator::_rxFromTx(uint32_t txId) {
    if (txId == 0x7DF) return 0x7E8;
    if (txId >= 0x7E0 && txId <= 0x7E7) return txId + 8;
    if (txId == 0x6F1) return 0x612;
    return txId + 8;
}

uint8_t ElmEmulator::_hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

uint16_t ElmEmulator::_hexToBytes(const char* hex, uint8_t* out, uint16_t maxOut) {
    uint16_t count = 0;
    while (*hex && count < maxOut) {
        while (*hex == ' ') hex++;
        if (!hex[0] || !hex[1]) break;
        out[count++] = (_hexNibble(hex[0]) << 4) | _hexNibble(hex[1]);
        hex += 2;
    }
    return count;
}
