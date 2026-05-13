#pragma once
// =============================================================================
//  BLE GATT Server – Binärprotokoll zum Display
//  Ersetzt ELM327 ASCII komplett durch effizientes Binary
// =============================================================================
#include <Arduino.h>
#include <NimBLEDevice.h>

// BLE Service/Characteristic UUIDs (Custom)
#define OBD_SERVICE_UUID    "0000FFE0-0000-1000-8000-00805F9B34FB"
#define OBD_CMD_UUID        "0000FFE1-0000-1000-8000-00805F9B34FB"  // Write: Befehle
#define OBD_RESP_UUID       "0000FFE2-0000-1000-8000-00805F9B34FB"  // Notify: Antworten
#define OBD_STATUS_UUID     "0000FFE3-0000-1000-8000-00805F9B34FB"  // Notify: Status

// Nordic UART Service (NUS) – ELM327/329 Emulation für Torque Pro etc.
#define NUS_SERVICE_UUID    "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID         "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Write: AT Befehle
#define NUS_TX_UUID         "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // Notify: Antworten

// ── Binärprotokoll Kommando-Typen ───────────────────────────────────────────
// Format: [CMD_TYPE] [payload...]
// Antwort: [CMD_TYPE] [STATUS] [payload...]
enum class OBDCmd : uint8_t {
    // OBD2 Standard (ISO 15031 / SAE J1979)
    READ_PID        = 0x01,   // [PID] → [SVC, PID, data...]
    READ_VIN        = 0x02,   // [] → [17 bytes VIN]
    READ_DTC        = 0x03,   // [] → [count, DTC1_hi, DTC1_lo, ...]
    CLEAR_DTC       = 0x04,   // [] → [OK/FAIL]
    READ_MULTI_PID  = 0x05,   // [PID1,PID2,...PID6] → [SVC, PID1,d..,PID2,d..]

    // PID-Discovery: Adapter pollt intern Mode-01 PIDs 0x00, 0x20, 0x40, 0x60,
    // 0x80, 0xA0, 0xC0 (jeder gibt 4-byte Bitmap der nächsten 32 supporteten
    // PIDs). Antwort: 28 byte Bitmap aller PIDs 0x01..0xE0.
    // Display ruft das einmal pro Connect/Auto auf und pollt danach nur die
    // PIDs die das Auto wirklich unterstützt — wie Torque Pro.
    DISCOVER_PIDS   = 0x06,   // [] → [28 byte bitmap, MSB-first per byte]

    // UDS-universal (jedes Auto mit UDS-fähigem ECU, NICHT BMW-spezifisch)
    READ_DID_22     = 0x10,   // [DID_hi, DID_lo] → [SVC=0x62, DID, data...]
    READ_DTC_UDS    = 0x12,   // [] → [count, DTCs...]
    CLEAR_DTC_UDS   = 0x13,   // [] → [OK/FAIL]
    DIAG_SESSION    = 0x14,   // [session_type] → [OK/FAIL]

    // Adapter-Steuerung
    GET_STATUS      = 0xF0,   // [] → [version, bus_type, connected]
    SET_BUS_CAN     = 0xF1,   // [baud_idx] → CAN Bus aktivieren
    SET_BUS_KLINE   = 0xF2,   // [] → K-Line aktivieren
    PING            = 0xFF,   // [] → [PONG]
};

enum class OBDStatus : uint8_t {
    OK              = 0x00,
    ERR_TIMEOUT     = 0x01,
    ERR_NO_RESP     = 0x02,
    ERR_NEGATIVE    = 0x03,   // UDS Negative Response
    ERR_BUS         = 0x04,   // CAN/K-Line Busfehler
    ERR_NOT_INIT    = 0x05,
};

// ── Callback für eingehende Befehle ─────────────────────────────────────────
typedef void (*BleCommandCb)(OBDCmd cmd, const uint8_t* data, uint8_t len);
typedef void (*BleDisconnectCb)();

class BleServer {
public:
    void begin(BleCommandCb cmdCallback);
    void setDisconnectCb(BleDisconnectCb cb) { _disconnectCb = cb; }
    void sendResponse(OBDCmd cmd, OBDStatus status,
                      const uint8_t* data = nullptr, uint16_t len = 0);
    void sendStatus(const char* msg);
    bool isConnected() const;

    // Command-Queue: BLE-Callback schreibt rein, loop() verarbeitet
    void processCommandQueue();

    // NUS (ELM327): ASCII-Daten senden + Queue verarbeiten
    typedef void (*NusDataCb)(const uint8_t* data, uint16_t len);
    void setNusCallback(NusDataCb cb) { _nusCb = cb; }
    void nusSend(const char* data, uint16_t len);
    void processNusQueue();

private:
    NimBLEServer*         _server  = nullptr;
    NimBLECharacteristic* _cmdChar = nullptr;
    NimBLECharacteristic* _respChar = nullptr;
    NimBLECharacteristic* _statusChar = nullptr;
    BleCommandCb          _cmdCb   = nullptr;
    BleDisconnectCb       _disconnectCb = nullptr;
    bool                  _connected = false;

    // Command-Queue (BLE Callback → Main Loop)
    static constexpr uint8_t CMD_QUEUE_SIZE = 8;
    static constexpr uint8_t CMD_MAX_LEN    = 64;
    struct CmdEntry {
        uint8_t data[CMD_MAX_LEN];
        uint8_t len;
    };
    CmdEntry         _cmdQueue[CMD_QUEUE_SIZE];
    volatile uint8_t _cmdHead = 0;
    volatile uint8_t _cmdTail = 0;

    // NUS (ELM327 emulation)
    NimBLECharacteristic* _nusTxChar = nullptr;
    NimBLECharacteristic* _nusRxChar = nullptr;
    NusDataCb             _nusCb = nullptr;

    static constexpr uint8_t NUS_QUEUE_SIZE = 8;
    static constexpr uint8_t NUS_MAX_LEN    = 240;
    struct NusEntry {
        uint8_t data[NUS_MAX_LEN];
        uint8_t len;
    };
    NusEntry         _nusQueue[NUS_QUEUE_SIZE];
    volatile uint8_t _nusHead = 0;
    volatile uint8_t _nusTail = 0;

    friend class ServerCallbacks;
    friend class CmdCharCallbacks;
    friend class NusRxCallbacks;
};
