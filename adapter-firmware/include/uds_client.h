#pragma once
// =============================================================================
//  UDS Client (ISO 14229) + OBD2 (SAE J1979)
//  Unterstützt alle BMW-relevanten Diagnose-Services
// =============================================================================
#include <Arduino.h>
#include "adapter_config.h"
#include "isotp.h"

// UDS Service IDs
#define UDS_READ_DID            0x22  // ReadDataByIdentifier
#define UDS_DYN_DEF_DID         0x2C  // DynamicallyDefineDataIdentifier (N47)
#define UDS_READ_DTC_INFO       0x19  // ReadDTCInformation
#define UDS_CLEAR_DTC           0x14  // ClearDiagnosticInformation
#define UDS_DIAG_SESSION        0x10  // DiagnosticSessionControl
#define UDS_TESTER_PRESENT      0x3E  // TesterPresent
#define UDS_ECU_RESET           0x11  // ECUReset

// OBD2 Service IDs
#define OBD2_CURRENT_DATA       0x01  // Mode 01: Current Data
#define OBD2_FREEZE_FRAME       0x02  // Mode 02: Freeze Frame
#define OBD2_READ_DTC           0x03  // Mode 03: Read DTCs
#define OBD2_CLEAR_DTC          0x04  // Mode 04: Clear DTCs
#define OBD2_VEHICLE_INFO       0x09  // Mode 09: Vehicle Information

// BMW ECU Adressen (Request → Response)
struct EcuAddr {
    uint32_t    txId;       // Request CAN-ID
    uint32_t    rxId;       // Response CAN-ID
    const char* name;
};

static const EcuAddr BMW_ECUS[] = {
    { 0x7E0, 0x7E8, "DME/DDE" },    // Motor
    { 0x7E1, 0x7E9, "EGS"     },    // Getriebe
    { 0x7E2, 0x7EA, "DSC"     },    // ESP/ABS
    { 0x7E3, 0x7EB, "EPS"     },    // Lenkung
    { 0x7E4, 0x7EC, "ACSM"    },    // Airbag
    { 0x7E5, 0x7ED, "IHKA"    },    // Klima
    { 0x7E6, 0x7EE, "FRM"     },    // Karosserie
    { 0x7E7, 0x7EF, "KOMBI"   },    // Kombiinstrument
};
static constexpr uint8_t BMW_ECU_COUNT = sizeof(BMW_ECUS) / sizeof(BMW_ECUS[0]);

// BMW Diagnose-Adressen (6F1→612 etc.)
static const EcuAddr BMW_DIAG_ECUS[] = {
    { 0x6F1, 0x612, "DME/DDE (Diag)" },
    { 0x6F1, 0x613, "EGS (Diag)"     },
    { 0x6F1, 0x614, "DSC (Diag)"     },
};

struct UdsResponse {
    uint8_t  data[512];
    uint16_t len;
    uint8_t  serviceId;    // Response SID (z.B. 0x62 für ReadDID)
    bool     negative;     // true = Negative Response (0x7F)
    uint8_t  nrc;          // Negative Response Code (nur wenn negative=true)
};

class UdsClient {
public:
    UdsClient(IsotpHandler& isotp) : _isotp(isotp) {}

    // ── OBD2 Standard (Mode 0x01-0x09) ────────────────────────────────────
    bool readPid(uint8_t pid, UdsResponse& resp,
                 uint32_t txId = OBD2_DME_REQ, uint32_t rxId = OBD2_DME_RESP);

    // Multi-PID: bis zu 6 PIDs in einer CAN-Anfrage (01 PID1 PID2 ... PID6)
    // ECU antwortet mit allen Werten in einem Frame: 41 PID1 data PID2 data ...
    bool readMultiPid(const uint8_t* pids, uint8_t count, UdsResponse& resp,
                      uint32_t txId = OBD2_DME_REQ, uint32_t rxId = OBD2_DME_RESP);

    bool readVin(char* vinBuf, uint8_t maxLen);
    bool readDtcsObd2(UdsResponse& resp);
    bool clearDtcsObd2();

    // ── UDS (BMW Diagnose) ────────────────────────────────────────────────
    bool readDid(uint16_t did, UdsResponse& resp,
                 uint32_t txId = OBD2_DME_REQ, uint32_t rxId = OBD2_DME_RESP);

    // N47 DDE7.1: Mode 0x2C mit 3-Byte DID (2C 10 XXXX)
    bool readDid2C(uint16_t did, UdsResponse& resp,
                   uint32_t txId = BMW_TESTER_ADDR, uint32_t rxId = BMW_DME_RESP);

    bool readDtcsUds(UdsResponse& resp,
                     uint32_t txId = OBD2_DME_REQ, uint32_t rxId = OBD2_DME_RESP);

    bool clearDtcsUds(uint32_t txId = OBD2_DME_REQ, uint32_t rxId = OBD2_DME_RESP);

    bool startDiagSession(uint8_t sessionType,
                          uint32_t txId = OBD2_DME_REQ, uint32_t rxId = OBD2_DME_RESP);

    bool testerPresent(uint32_t txId = OBD2_DME_REQ, uint32_t rxId = OBD2_DME_RESP);

private:
    IsotpHandler& _isotp;

    bool _sendAndReceive(const uint8_t* req, uint16_t reqLen,
                         UdsResponse& resp,
                         uint32_t txId, uint32_t rxId,
                         uint32_t timeoutMs = UDS_RESP_TIMEOUT_MS);
};
