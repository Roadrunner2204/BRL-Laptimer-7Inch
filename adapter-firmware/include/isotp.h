#pragma once
// =============================================================================
//  ISO-TP (ISO 15765-2) Transport Protocol Handler
//  Vollständige Implementierung: SF, FF, CF, FC
//
//  Plus BMW Extended Addressing (F-Series + späte E-Series DDE/DSC/EGS):
//  TX-Frames an 0x6F1 erhalten ein Target-Byte vor dem PCI;
//  RX-Frames auf 0x612 haben ein Source-Byte (0xF1) vor dem PCI.
// =============================================================================
#include <Arduino.h>
#include "driver/twai.h"

// ISO-TP Frame-Typen (obere 4 Bit von Byte 0)
#define ISOTP_SF   0x00   // Single Frame
#define ISOTP_FF   0x10   // First Frame
#define ISOTP_CF   0x20   // Consecutive Frame
#define ISOTP_FC   0x30   // Flow Control

// Flow Control Status
#define FC_CTS     0x00   // Continue To Send
#define FC_WAIT    0x01   // Wait
#define FC_ABORT   0x02   // Overflow / Abort

static constexpr uint16_t ISOTP_MAX_LEN = 4095;  // Max ISO-TP Payload

struct IsotpMessage {
    uint32_t canId;
    uint8_t  data[512];   // Reassemblierter Payload
    uint16_t len;          // Tatsächliche Datenlänge
    bool     complete;
};

class IsotpHandler {
public:
    // Sendet eine ISO-TP Nachricht (automatisch SF oder FF+CF)
    // txId: CAN-ID zum Senden
    // rxId: CAN-ID für Flow Control Empfang
    bool send(uint32_t txId, uint32_t rxId,
              const uint8_t* data, uint16_t len);

    // Empfängt einen CAN-Frame und baut ISO-TP Nachricht zusammen
    // Gibt true zurück wenn Nachricht komplett
    bool receive(const twai_message_t& frame, IsotpMessage& msg);

    // Sendet Flow Control Frame
    void sendFlowControl(uint32_t txId, uint8_t status = FC_CTS,
                         uint8_t blockSize = 0, uint8_t stMin = 0);

    // BMW Extended Addressing.
    // enabled=true → bei jedem TX-Frame wird _bmwTarget vor das PCI-Byte
    // geschrieben; beim RX wird das erste Datenbyte als Source verworfen.
    // targetByte: 0x12 = DDE/DME, 0x18 = EGS, 0x29 = DSC, 0x40 = ZGW etc.
    void setBmwExtended(bool enabled, uint8_t targetByte = 0x12);
    bool isBmwExtended() const { return _bmwExt; }

    // Reset des Empfangspuffers
    void reset();

private:
    bool    _bmwExt    = false;
    uint8_t _bmwTarget = 0x12;

    // Empfangs-State
    uint8_t  _rxBuf[512];
    uint16_t _rxExpected = 0;
    uint16_t _rxGot      = 0;
    uint8_t  _rxSeq      = 0;
    uint32_t _rxCanId    = 0;
    uint32_t _lastRxTime = 0;
    bool     _rxActive   = false;

    // Sende-State (für Multi-Frame mit FC)
    uint8_t  _txBuf[512];
    uint16_t _txLen      = 0;
    uint16_t _txSent     = 0;
    uint8_t  _txSeq      = 0;
    uint32_t _txCanId    = 0;
    uint32_t _fcRxId     = 0;
    bool     _txWaitFC   = false;
    uint8_t  _fcBlockSize = 0;
    uint8_t  _fcStMin    = 0;
    uint8_t  _fcBlockCnt = 0;

    bool _sendSingleFrame(uint32_t txId, const uint8_t* data, uint16_t len);
    bool _sendFirstFrame(uint32_t txId, const uint8_t* data, uint16_t len);
    bool _sendConsecutiveFrames(uint32_t txId);
    bool _txCanFrame(uint32_t canId, const uint8_t* data, uint8_t dlc);
};
