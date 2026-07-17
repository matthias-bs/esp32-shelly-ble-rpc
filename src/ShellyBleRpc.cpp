/**
 * @file ShellyBleRpc.cpp
 * @brief Implementation of the ShellyBleRpc library.
 *
 * License: MIT
 */

#include "ShellyBleRpc.h"
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Static member initialisation
// ---------------------------------------------------------------------------
ShellyBleRpc* ShellyBleRpc::_instance = nullptr;

// ---------------------------------------------------------------------------
// ClientCallbacks
// ---------------------------------------------------------------------------

void ShellyBleRpc::ClientCallbacks::onConnect(NimBLEClient* pClient) {
    if (_parent->_debug) {
        Serial.printf("[ShellyBleRpc] Connected to %s\n",
                      pClient->getPeerAddress().toString().c_str());
    }
}

void ShellyBleRpc::ClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    if (_parent->_debug) {
        Serial.printf("[ShellyBleRpc] Disconnected from %s (reason: %d)\n",
                      pClient->getPeerAddress().toString().c_str(), reason);
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

ShellyBleRpc::ShellyBleRpc()
    : _pClient(nullptr),
      _pTxChar(nullptr),
      _pRxChar(nullptr),
      _pCallbacks(nullptr),
      _debug(false),
      _timeoutMs(SHELLY_BLE_RPC_DEFAULT_TIMEOUT_MS),
      _initDone(false),
      _rpcId(1)
{
    _responseSemaphore = xSemaphoreCreateBinary();
    _instance = this;
}

ShellyBleRpc::~ShellyBleRpc() {
    disconnect();
    delete _pCallbacks;
    _pCallbacks = nullptr;
    if (_responseSemaphore) {
        vSemaphoreDelete(_responseSemaphore);
        _responseSemaphore = nullptr;
    }
    if (_instance == this) {
        _instance = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ShellyBleRpc::begin(const char* deviceName) {
    if (!_initDone) {
        NimBLEDevice::init(deviceName);
        // Request a larger ATT MTU so that fewer BLE fragments are needed
        // per RPC message (the actual MTU is negotiated with the peer).
        NimBLEDevice::setMTU(517);
        _initDone = true;
    }
    if (!_pCallbacks) {
        _pCallbacks = new ClientCallbacks(this);
    }
    return true;
}

bool ShellyBleRpc::connect(const char* address, uint8_t addressType) {
    return connect(NimBLEAddress(std::string(address), addressType));
}

bool ShellyBleRpc::connect(const NimBLEAddress& address) {
    if (!_initDone) {
        _debugLog("Call begin() before connect()");
        return false;
    }

    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        pScan->stop();
    }

    // Reuse an existing NimBLEClient if one was already created.
    if (_pClient == nullptr) {
        _pClient = NimBLEDevice::createClient();
        _pClient->setClientCallbacks(_pCallbacks, false);
    }

    _debugLog("Connecting to %s ...", address.toString().c_str());

    if (!_pClient->connect(address)) {
        _debugLog("Connection failed");
        return false;
    }

    _debugLog("Connected – setting up RPC service ...");

    if (!_setupService()) {
        _debugLog("RPC service setup failed – disconnecting");
        _pClient->disconnect();
        return false;
    }

    _debugLog("Ready (MTU=%d)", _pClient->getMTU());
    return true;
}

bool ShellyBleRpc::scan(uint32_t durationMs, const char* nameFilter) {
    if (!_initDone) {
        _debugLog("Call begin() before scan()");
        return false;
    }
    if (isConnected()) {
        _debugLog("Disconnect before scan()");
        return false;
    }

    _scanResults.clear();

    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
        _debugLog("Failed to get NimBLE scanner");
        return false;
    }

    pScan->stop();
    pScan->clearResults();
    pScan->setInterval(100);
    pScan->setWindow(100);
    pScan->setActiveScan(true);

    _debugLog("Scanning for Shelly RPC service for %lu ms ...", durationMs);

    NimBLEScanResults results =
        pScan->getResults(durationMs > 0 ? durationMs : SHELLY_BLE_RPC_DEFAULT_SCAN_MS,
                          false);
    NimBLEUUID serviceUuid(SHELLY_BLE_RPC_SERVICE_UUID);

    for (int i = 0; i < results.getCount(); ++i) {
        const NimBLEAdvertisedDevice* device = results.getDevice(i);
        if (!device) {
            continue;
        }

        String name = device->haveName() ? String(device->getName().c_str()) : String();

        // A device matches if it advertises the Shelly RPC service UUID, or if
        // its name starts with "Shelly" (Shelly devices do not always include
        // the service UUID in their advertisement, e.g. when in pairing mode).
        bool hasServiceUuid = device->isAdvertisingService(serviceUuid);
        bool hasShellyName  = name.length() >= 6 &&
                              name.substring(0, 6).equalsIgnoreCase("Shelly");
        if (!hasServiceUuid && !hasShellyName) {
            continue;
        }

        if (nameFilter && nameFilter[0] != '\0' && !name.equals(nameFilter)) {
            continue;
        }

        ScanResult result;
        result.address     = String(device->getAddress().toString().c_str());
        result.name        = name;
        result.rssi        = device->getRSSI();
        result.addressType = device->getAddress().getType();
        _scanResults.push_back(result);

        _debugLog("Scan hit: %s RSSI=%d name='%s' type=%u",
                  result.address.c_str(),
                  result.rssi,
                  result.name.length() ? result.name.c_str() : "(none)",
                  result.addressType);
    }

    pScan->clearResults();
    _debugLog("Scan complete: %u matching device(s)",
              static_cast<unsigned>(_scanResults.size()));
    return !_scanResults.empty();
}

size_t ShellyBleRpc::getScanResultCount() const {
    return _scanResults.size();
}

bool ShellyBleRpc::getScanResult(size_t index, ScanResult& result) const {
    if (index >= _scanResults.size()) {
        return false;
    }
    result = _scanResults[index];
    return true;
}

bool ShellyBleRpc::connectToScanResult(size_t index) {
    if (index >= _scanResults.size()) {
        _debugLog("Scan result index out of range");
        return false;
    }

    const ScanResult& result = _scanResults[index];
    _debugLog("Connecting to scan result %u: %s",
              static_cast<unsigned>(index), result.address.c_str());
    return connect(result.address.c_str(), result.addressType);
}

bool ShellyBleRpc::scanAndConnect(uint32_t durationMs, const char* nameFilter) {
    if (!scan(durationMs, nameFilter)) {
        return false;
    }

    size_t bestIndex = 0;
    int    bestRssi  = _scanResults[0].rssi;

    for (size_t i = 1; i < _scanResults.size(); ++i) {
        if (_scanResults[i].rssi > bestRssi) {
            bestRssi  = _scanResults[i].rssi;
            bestIndex = i;
        }
    }

    _debugLog("Connecting to strongest scan result %u (RSSI=%d)",
              static_cast<unsigned>(bestIndex), bestRssi);
    return connectToScanResult(bestIndex);
}

void ShellyBleRpc::disconnect() {
    if (_pClient && _pClient->isConnected()) {
        _pClient->disconnect();
    }
    _pTxChar = nullptr;
    _pRxChar = nullptr;
}

bool ShellyBleRpc::isConnected() const {
    return _pClient && _pClient->isConnected();
}

// ---------------------------------------------------------------------------
// Internal – service / characteristic setup
// ---------------------------------------------------------------------------

bool ShellyBleRpc::_setupService() {
    NimBLERemoteService* pSvc =
        _pClient->getService(SHELLY_BLE_RPC_SERVICE_UUID);
    if (!pSvc) {
        _debugLog("RPC service not found (UUID: %s)", SHELLY_BLE_RPC_SERVICE_UUID);
        return false;
    }

    _pTxChar = pSvc->getCharacteristic(SHELLY_BLE_RPC_TX_CHAR_UUID);
    if (!_pTxChar) {
        _debugLog("TX characteristic not found (UUID: %s)", SHELLY_BLE_RPC_TX_CHAR_UUID);
        return false;
    }
    if (!_pTxChar->canWriteNoResponse() && !_pTxChar->canWrite()) {
        _debugLog("TX characteristic is not writable");
        return false;
    }

    _pRxChar = pSvc->getCharacteristic(SHELLY_BLE_RPC_RX_CHAR_UUID);
    if (!_pRxChar) {
        _debugLog("RX characteristic not found (UUID: %s)", SHELLY_BLE_RPC_RX_CHAR_UUID);
        return false;
    }
    if (!_pRxChar->canNotify()) {
        _debugLog("RX characteristic does not support notifications");
        return false;
    }

    if (!_pRxChar->subscribe(true, _staticNotifyCallback)) {
        _debugLog("Failed to register for notifications on RX characteristic");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Internal – fragmented write
// ---------------------------------------------------------------------------

void ShellyBleRpc::_sendFragmented(const uint8_t* data, size_t length) {
    // ATT MTU overhead is 3 bytes (opcode + handle), so the usable payload
    // per write is (MTU - 3).  One additional byte is consumed by the mOS
    // RPC fragment-count header, leaving (MTU - 4) bytes of RPC data per
    // write.  A conservative floor of 19 bytes is applied when MTU < 23.
    uint16_t mtu = _pClient->getMTU();
    size_t maxPayload = (mtu > 3) ? static_cast<size_t>(mtu - 3) : 20u;
    size_t chunkData  = (maxPayload > 1) ? (maxPayload - 1) : 1u;

    size_t numChunks = (length + chunkData - 1) / chunkData;

    for (size_t i = 0; i < numChunks; i++) {
        size_t offset       = i * chunkData;
        size_t thisDataSize = (offset + chunkData <= length)
                                  ? chunkData
                                  : (length - offset);

        // Fragment format: [remaining_after_this (1 byte)] [data ...]
        size_t   chunkLen = thisDataSize + 1;
        uint8_t* chunk    = new uint8_t[chunkLen];
        chunk[0] = static_cast<uint8_t>(numChunks - 1 - i);
        memcpy(&chunk[1], &data[offset], thisDataSize);

        // Prefer Write Without Response for throughput; fall back to Write.
        bool useResponse = !_pTxChar->canWriteNoResponse();
        _pTxChar->writeValue(chunk, chunkLen, useResponse);

        delete[] chunk;

        // Brief inter-fragment delay to avoid overflowing the peer's RX buffer.
        if (i + 1 < numChunks) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ---------------------------------------------------------------------------
// Internal – notification handler
// ---------------------------------------------------------------------------

void ShellyBleRpc::_staticNotifyCallback(NimBLERemoteCharacteristic* /*pChar*/,
                                          uint8_t* pData, size_t length,
                                          bool /*isNotify*/) {
    if (_instance) {
        _instance->_handleNotification(pData, length);
    }
}

void ShellyBleRpc::_handleNotification(const uint8_t* pData, size_t length) {
    if (length == 0) return;

    uint8_t remaining = pData[0];

    // Append the fragment payload (everything after the 1-byte header).
    for (size_t i = 1; i < length; i++) {
        if (_responseBuffer.length() < SHELLY_BLE_RPC_BUFFER_SIZE) {
            _responseBuffer += static_cast<char>(pData[i]);
        }
    }

    _debugLog("Notify: remaining=%u, bufLen=%u", remaining,
              static_cast<unsigned>(_responseBuffer.length()));

    if (remaining == 0) {
        // Last (or only) fragment – signal the waiting call().
        xSemaphoreGive(_responseSemaphore);
    }
}

// ---------------------------------------------------------------------------
// Generic RPC call
// ---------------------------------------------------------------------------

bool ShellyBleRpc::call(const char* method, const char* params,
                         String& response, uint32_t timeoutMs) {
    if (!isConnected()) {
        _debugLog("Not connected");
        return false;
    }
    if (!_pTxChar || !_pRxChar) {
        _debugLog("Service not set up");
        return false;
    }

    // Build the JSON-RPC 2.0 request.
    String request;
    request.reserve(128);
    request += "{\"id\":";
    request += _rpcId++;
    request += ",\"method\":\"";
    request += method;
    request += "\",\"params\":";
    const char* p = (params && params[0] != '\0') ? params : "{}";
    request += p;
    request += "}";

    _debugLog("RPC >> %s", request.c_str());

    // Drain any leftover semaphore signal from a previous (timed-out) call.
    xSemaphoreTake(_responseSemaphore, 0);
    _responseBuffer = "";

    // Send the request in BLE fragments.
    _sendFragmented(reinterpret_cast<const uint8_t*>(request.c_str()),
                    request.length());

    // Block until the last response fragment arrives or timeout.
    if (xSemaphoreTake(_responseSemaphore,
                        pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        _debugLog("RPC timeout (%lu ms) for method %s", timeoutMs, method);
        return false;
    }

    response = _responseBuffer;
    _debugLog("RPC << %s", response.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Shelly convenience methods
// ---------------------------------------------------------------------------

bool ShellyBleRpc::shellyGetDeviceInfo(String& response) {
    return call("Shelly.GetDeviceInfo", "{}", response);
}

bool ShellyBleRpc::shellyGetStatus(String& response) {
    return call("Shelly.GetStatus", "{}", response);
}

bool ShellyBleRpc::shellyGetConfig(String& response) {
    return call("Shelly.GetConfig", "{}", response);
}

bool ShellyBleRpc::shellyReboot(String& response) {
    return call("Shelly.Reboot", "{\"delay_ms\":500}", response);
}

bool ShellyBleRpc::switchGet(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Switch.Get", params, response);
}

bool ShellyBleRpc::switchSet(uint8_t id, bool state, String& response) {
    char params[40];
    snprintf(params, sizeof(params), "{\"id\":%u,\"on\":%s}",
             id, state ? "true" : "false");
    return call("Switch.Set", params, response);
}

bool ShellyBleRpc::switchToggle(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Switch.Toggle", params, response);
}

bool ShellyBleRpc::inputGet(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Input.Get", params, response);
}

bool ShellyBleRpc::temperatureGet(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Temperature.Get", params, response);
}

bool ShellyBleRpc::humidityGet(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Humidity.Get", params, response);
}

bool ShellyBleRpc::coverOpen(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Cover.Open", params, response);
}

bool ShellyBleRpc::coverClose(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Cover.Close", params, response);
}

bool ShellyBleRpc::coverStop(uint8_t id, String& response) {
    char params[24];
    snprintf(params, sizeof(params), "{\"id\":%u}", id);
    return call("Cover.Stop", params, response);
}

bool ShellyBleRpc::coverGoToPosition(uint8_t id, uint8_t pos, String& response) {
    char params[40];
    snprintf(params, sizeof(params), "{\"id\":%u,\"pos\":%u}", id, pos);
    return call("Cover.GoToPosition", params, response);
}

// ---------------------------------------------------------------------------
// Configuration helpers
// ---------------------------------------------------------------------------

void ShellyBleRpc::setDebug(bool enable) {
    _debug = enable;
}

void ShellyBleRpc::setTimeout(uint32_t timeoutMs) {
    _timeoutMs = timeoutMs;
}

uint16_t ShellyBleRpc::getMTU() const {
    return _pClient ? _pClient->getMTU() : 23u;
}

// ---------------------------------------------------------------------------
// Debug logging
// ---------------------------------------------------------------------------

void ShellyBleRpc::_debugLog(const char* fmt, ...) {
    if (!_debug) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print("[ShellyBleRpc] ");
    Serial.println(buf);
}
