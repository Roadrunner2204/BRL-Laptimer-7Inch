#pragma once
// =============================================================================
//  ELM327/ELM329 Emulator – ASCII-Interface für Third-Party Apps
//  Torque Pro, BimmerLink, Car Scanner etc. über BLE UART (NUS)
// =============================================================================
#include <Arduino.h>
#include "uds_client.h"
#include "isotp.h"

#define ELM_VERSION   "ELM327 v2.1"
#define ELM329_VERSION "ELM329 v2.0"
#define ELM_DEVICE    "BRL OBD Adapter"

enum class ElmProtocol : uint8_t {
    AUTO = 0, CAN_500_11 = 6, CAN_500_29 = 7, CAN_250_11 = 8, CAN_250_29 = 9,
};

struct ElmConfig {
    bool        echo      = true;
    bool        linefeed  = true;
    bool        spaces    = true;
    bool        headers   = false;
    ElmProtocol protocol  = ElmProtocol::CAN_500_11;
    uint32_t    canHeader = 0x7DF;
    uint8_t     timeout   = 0x32;   // ×4ms = 200ms
    bool        canFlowCtrl = true;
    bool        canAutoFmt  = true;
};

typedef void (*ElmSendCb)(const char* data, uint16_t len);
typedef bool (*ElmBusInitCb)(uint32_t baudRate);

class ElmEmulator {
public:
    ElmEmulator(UdsClient& uds, IsotpHandler& isotp);

    void setSendCallback(ElmSendCb cb) { _sendCb = cb; }
    void setBusInitCallback(ElmBusInitCb cb) { _busInitCb = cb; }

    // Empfange Rohdaten vom NUS RX (können Teilzeilen sein)
    void feed(const uint8_t* data, uint16_t len);

    void update();

private:
    UdsClient&    _uds;
    IsotpHandler& _isotp;
    ElmSendCb     _sendCb    = nullptr;
    ElmBusInitCb  _busInitCb = nullptr;
    ElmConfig     _cfg;

    char     _lineBuf[256];
    uint16_t _linePos = 0;

    void _processLine(const char* line);
    void _handleAT(const char* cmd);
    void _handleOBD(const char* hex);

    // OBD Modes
    void _execMode01(const uint8_t* pids, uint8_t count);
    void _execMode03();
    void _execMode04();
    void _execMode09(uint8_t pid);
    void _execRawUDS(const uint8_t* data, uint8_t len);

    // Response formatting
    void _send(const char* str);
    void _sendLine(const char* str);
    void _sendOK();
    void _sendError();
    void _sendNoData();
    void _sendPrompt();
    void _formatHexResponse(const uint8_t* data, uint16_t len, uint32_t rxId);

    // Helpers
    uint32_t _rxFromTx(uint32_t txId);
    uint8_t  _hexNibble(char c);
    uint16_t _hexToBytes(const char* hex, uint8_t* out, uint16_t maxOut);
};
