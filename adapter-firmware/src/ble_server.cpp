// =============================================================================
//  BLE GATT Server – NimBLE Implementation
// =============================================================================
#include "ble_server.h"
#include "adapter_config.h"

// ── NimBLE Callbacks ────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
public:
    ServerCallbacks(BleServer* parent) : _parent(parent) {}
    void onConnect(NimBLEServer* server) override {
        _parent->_connected = true;
        Serial.println("[BLE] Display connected");
    }
    void onDisconnect(NimBLEServer* server) override {
        _parent->_connected = false;
        Serial.println("[BLE] Display disconnected");
        if (_parent->_disconnectCb) _parent->_disconnectCb();
        NimBLEDevice::startAdvertising();
    }
private:
    BleServer* _parent;
};

class CmdCharCallbacks : public NimBLECharacteristicCallbacks {
public:
    CmdCharCallbacks(BleServer* parent) : _parent(parent) {}
    void onWrite(NimBLECharacteristic* ch) override {
        // NICHT direkt verarbeiten! BLE-Stack blockiert sonst bei langen CAN-Ops.
        // Stattdessen in Queue schreiben → loop() verarbeitet.
        std::string val = ch->getValue();
        if (val.size() < 1) return;
        uint8_t nextHead = (_parent->_cmdHead + 1) % BleServer::CMD_QUEUE_SIZE;
        if (nextHead != _parent->_cmdTail) {
            uint8_t copyLen = (val.size() > BleServer::CMD_MAX_LEN)
                            ? BleServer::CMD_MAX_LEN : val.size();
            memcpy(_parent->_cmdQueue[_parent->_cmdHead].data, val.data(), copyLen);
            _parent->_cmdQueue[_parent->_cmdHead].len = copyLen;
            _parent->_cmdHead = nextHead;
        }
    }
private:
    BleServer* _parent;
};

class NusRxCallbacks : public NimBLECharacteristicCallbacks {
public:
    NusRxCallbacks(BleServer* parent) : _parent(parent) {}
    void onWrite(NimBLECharacteristic* ch) override {
        std::string val = ch->getValue();
        if (val.size() < 1) return;
        uint8_t nextHead = (_parent->_nusHead + 1) % BleServer::NUS_QUEUE_SIZE;
        if (nextHead != _parent->_nusTail) {
            uint8_t copyLen = (val.size() > BleServer::NUS_MAX_LEN)
                            ? BleServer::NUS_MAX_LEN : val.size();
            memcpy(_parent->_nusQueue[_parent->_nusHead].data, val.data(), copyLen);
            _parent->_nusQueue[_parent->_nusHead].len = copyLen;
            _parent->_nusHead = nextHead;
        }
    }
private:
    BleServer* _parent;
};

// ── BLE Server Setup ────────────────────────────────────────────────────────
void BleServer::begin(BleCommandCb cmdCallback) {
    _cmdCb = cmdCallback;

    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);  // Public Addr für Bluedroid-Kompatibilität
    NimBLEDevice::setSecurityAuth(false, false, false);  // Kein Bonding/MITM

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new ServerCallbacks(this));

    NimBLEService* svc = _server->createService(OBD_SERVICE_UUID);

    // Command Characteristic (Write)
    _cmdChar = svc->createCharacteristic(
        OBD_CMD_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _cmdChar->setCallbacks(new CmdCharCallbacks(this));

    // Response Characteristic (Notify)
    _respChar = svc->createCharacteristic(
        OBD_RESP_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );

    // Status Characteristic (Notify)
    _statusChar = svc->createCharacteristic(
        OBD_STATUS_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );

    svc->start();

    // ── NUS Service (ELM327/329 Emulation) ──────────────────────────────
    NimBLEService* nusSvc = _server->createService(NUS_SERVICE_UUID);

    _nusRxChar = nusSvc->createCharacteristic(
        NUS_RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _nusRxChar->setCallbacks(new NusRxCallbacks(this));

    _nusTxChar = nusSvc->createCharacteristic(
        NUS_TX_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );

    nusSvc->start();
    Serial.println("[BLE] NUS Service gestartet (ELM327/329)");

    _server->advertiseOnDisconnect(true);

    // Einfachstes mögliches Advertising – nur Name, keine UUID
    NimBLEDevice::getAdvertising()->start();

    Serial.printf("[BLE] Advertising gestartet: '%s'\n", BLE_DEVICE_NAME);
}

void BleServer::sendResponse(OBDCmd cmd, OBDStatus status,
                              const uint8_t* data, uint16_t len) {
    if (!_connected || !_respChar) return;

    uint8_t buf[512];
    buf[0] = (uint8_t)cmd;
    buf[1] = (uint8_t)status;
    uint16_t total = 2;
    if (data && len > 0) {
        uint16_t copyLen = (len > 510) ? 510 : len;
        memcpy(buf + 2, data, copyLen);
        total += copyLen;
    }
    _respChar->setValue(buf, total);
    _respChar->notify();
}

void BleServer::sendStatus(const char* msg) {
    if (!_connected || !_statusChar) return;
    _statusChar->setValue((const uint8_t*)msg, strlen(msg));
    _statusChar->notify();
}

bool BleServer::isConnected() const {
    return _connected;
}

void BleServer::nusSend(const char* data, uint16_t len) {
    if (!_connected || !_nusTxChar || !data || len == 0) return;
    uint16_t offset = 0;
    while (offset < len) {
        uint16_t chunk = (len - offset > 240) ? 240 : (len - offset);
        _nusTxChar->setValue((const uint8_t*)(data + offset), chunk);
        _nusTxChar->notify();
        offset += chunk;
    }
}

void BleServer::processNusQueue() {
    if (_nusHead == _nusTail) return;
    uint8_t* raw = _nusQueue[_nusTail].data;
    uint8_t  len = _nusQueue[_nusTail].len;
    _nusTail = (_nusTail + 1) % NUS_QUEUE_SIZE;
    if (_nusCb) _nusCb(raw, len);
}

void BleServer::processCommandQueue() {
    // Maximal 1 Befehl pro Aufruf verarbeiten (CAN-Ops können blockieren)
    if (_cmdHead == _cmdTail) return;  // Queue leer

    uint8_t* raw = _cmdQueue[_cmdTail].data;
    uint8_t  len = _cmdQueue[_cmdTail].len;
    _cmdTail = (_cmdTail + 1) % CMD_QUEUE_SIZE;

    if (len >= 1 && _cmdCb) {
        OBDCmd cmd = (OBDCmd)raw[0];
        const uint8_t* data = (len > 1) ? raw + 1 : nullptr;
        uint8_t dataLen = (len > 1) ? len - 1 : 0;
        _cmdCb(cmd, data, dataLen);
    }
}
