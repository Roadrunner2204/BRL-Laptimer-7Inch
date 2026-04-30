#pragma once
// =============================================================================
//  K-Line Handler (ISO 9141-2 / KWP2000 / ISO 14230)
//  Für BMW E36, E38, E39, E46, E53 (ADS/INPA Diagnose)
// =============================================================================
#include <Arduino.h>

class KLineHandler {
public:
    // Initialisiert K-Line UART
    bool begin(uint8_t txPin, uint8_t rxPin);

    // ISO 9141-2 Slow Init (5-Baud Adresse senden, z.B. 0x33)
    bool slowInit(uint8_t targetAddr = 0x33);

    // KWP2000 Fast Init (Wakeup Pattern)
    bool fastInit();

    // KWP2000 Request senden und Antwort empfangen
    // request/response beinhalten nur Payload (ohne Header/Checksum)
    bool sendRequest(uint8_t targetAddr, const uint8_t* data, uint8_t len,
                     uint8_t* response, uint8_t* respLen, uint8_t maxRespLen,
                     uint32_t timeoutMs = 500);

    // OBD2 über K-Line (ISO 9141-2)
    bool readPidKLine(uint8_t pid, uint8_t* data, uint8_t* dataLen);

    // Verbindung aktiv?
    bool isConnected() const { return _connected; }

    // Keepalive senden (TesterPresent)
    void keepAlive();

private:
    HardwareSerial* _serial  = nullptr;
    bool            _connected = false;
    uint32_t        _lastComm  = 0;

    // 5-Baud Init: sendet ein Byte bei 5 Baud (200ms pro Bit)
    void _send5Baud(uint8_t byte);

    // KWP2000 Frame senden: [Format][Target][Source][Data...][Checksum]
    void _sendKwpFrame(uint8_t target, uint8_t source,
                       const uint8_t* data, uint8_t len);

    // KWP2000 Frame empfangen
    bool _recvKwpFrame(uint8_t* data, uint8_t* len, uint8_t maxLen,
                       uint32_t timeoutMs);

    uint8_t _checksum(const uint8_t* data, uint8_t len);
};
