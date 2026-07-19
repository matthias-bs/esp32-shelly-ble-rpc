/**
 * @file ShellyBleRpc.cpp
 * @brief Implementation of the ShellyBleRpc library.
 *
 * License: MIT
 */

#include "ShellyBleRpc.h"
#include <stdarg.h>

namespace {
constexpr uint8_t  kConnectAttempts = 4;
constexpr uint16_t kConnectBackoffMs[] = {250, 500, 1000};
constexpr uint8_t  kRpcRetryAttempts = 3;
constexpr uint16_t kRpcRetryBackoffMs[] = {150, 400};
constexpr uint16_t kReadEmptyDelayMs = 10;

uint16_t retryJitterMs() {
    static uint32_t lcg = 0xA5A5A5A5u;
    lcg = (lcg * 1664525u) + 1013904223u + static_cast<uint32_t>(micros());
    return static_cast<uint16_t>(lcg % 101u);
}

uint32_t connectRetryDelayMs(uint8_t attempt) {
    if (attempt >= kConnectAttempts) {
        return 0;
    }

    size_t index = static_cast<size_t>(attempt - 1);
    if (index >= (sizeof(kConnectBackoffMs) / sizeof(kConnectBackoffMs[0]))) {
        index = (sizeof(kConnectBackoffMs) / sizeof(kConnectBackoffMs[0])) - 1;
    }
    return static_cast<uint32_t>(kConnectBackoffMs[index]) + retryJitterMs();
}

uint32_t rpcRetryDelayMs(uint8_t retryNumber) {
    if (retryNumber == 0) {
        return 0;
    }

    size_t index = static_cast<size_t>(retryNumber - 1);
    if (index >= (sizeof(kRpcRetryBackoffMs) / sizeof(kRpcRetryBackoffMs[0]))) {
        index = (sizeof(kRpcRetryBackoffMs) / sizeof(kRpcRetryBackoffMs[0])) - 1;
    }
    return static_cast<uint32_t>(kRpcRetryBackoffMs[index]);
}
}  // namespace

// ---------------------------------------------------------------------------
// Static member initialisation
// ---------------------------------------------------------------------------
ShellyBleRpc* ShellyBleRpc::_instance = nullptr;

// ---------------------------------------------------------------------------
// ClientCallbacks
// ---------------------------------------------------------------------------

void ShellyBleRpc::ClientCallbacks::onConnect(NimBLEClient* pClient) {
    if (_parent->_debug) {
        log_i("Connected to %s", pClient->getPeerAddress().toString().c_str());
    }
}

void ShellyBleRpc::ClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    if (_parent->_debug) {
        log_i("Disconnected from %s (reason: %d)",
              pClient->getPeerAddress().toString().c_str(), reason);
    }
}

void ShellyBleRpc::ClientCallbacks::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    if (_parent->_debug) {
        log_i("Authentication %s (encrypted: %s)",
              connInfo.isEncrypted() ? "succeeded" : "failed",
              connInfo.isEncrypted() ? "yes" : "no");
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

ShellyBleRpc::ShellyBleRpc()
    : _pClient(nullptr),
      _pDataChar(nullptr),
      _pTxChar(nullptr),
      _pRxChar(nullptr),
      _pCallbacks(nullptr),
      _debug(false),
      _timeoutMs(SHELLY_BLE_RPC_DEFAULT_TIMEOUT_MS),
      _initDone(false),
      _rpcId(1),
      _responseLength(0),
      _responseLengthReady(false)
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

        // Configure "Just Works" BLE bonding so that Shelly devices in
        // pairing mode can complete authentication before GATT operations.
        // Bonding=true, MITM=false, SC=false → no user interaction required.
        NimBLEDevice::setSecurityAuth(true, false, false);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

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

    for (uint8_t attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        _debugLog("Connecting to %s (attempt %u/%u) ...",
                  address.toString().c_str(),
                  static_cast<unsigned>(attempt),
                  static_cast<unsigned>(kConnectAttempts));

        if (!_pClient->connect(address)) {
            _debugLog("Connection failed");
            if (attempt < kConnectAttempts) {
                delay(connectRetryDelayMs(attempt));
                continue;
            }
            return false;
        }

        // Proactively establish BLE security before any GATT operations.
        // When the ESP32 has previously bonded with this Shelly (keys stored in
        // NVS), NimBLE will use the Long-Term Key to encrypt the link without
        // requiring the user to enable pairing mode again.  On first use the
        // Shelly must have "Bluetooth pairing" enabled in the Shelly app or web
        // UI so that the pairing can complete.  If security is not required by
        // the device, this call is a no-op from the user's perspective.
        _debugLog("Establishing BLE security ...");
        if (!_pClient->secureConnection()) {
            if (!_pClient->isConnected()) {
                _debugLog("Connection dropped during security handshake; retrying");
                if (attempt < kConnectAttempts) {
                    delay(connectRetryDelayMs(attempt));
                    continue;
                }
                return false;
            }

            _debugLog("BLE security not established – if the Shelly requires "
                      "authentication, enable Bluetooth pairing in the Shelly "
                      "app / web UI and reconnect to bond the devices");
        }

        _debugLog("Setting up RPC service ...");

        if (_setupService()) {
            _debugLog("Ready (MTU=%d)", _pClient->getMTU());
            return true;
        }

        if (!_pClient->isConnected()) {
            _debugLog("Disconnected before RPC service setup completed");
        } else {
            _debugLog("RPC service setup failed – disconnecting");
            _pClient->disconnect();
        }

        if (attempt < kConnectAttempts) {
            delay(connectRetryDelayMs(attempt));
        }
    }

    return false;
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
    _pDataChar = nullptr;
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

    _pDataChar = pSvc->getCharacteristic(SHELLY_BLE_RPC_DATA_CHAR_UUID);
    if (!_pDataChar) {
        _debugLog("Data characteristic not found (UUID: %s)", SHELLY_BLE_RPC_DATA_CHAR_UUID);
        return false;
    }
    if (!_pDataChar->canRead()) {
        _debugLog("Data characteristic is not readable");
        return false;
    }
    if (!_pDataChar->canWriteNoResponse() && !_pDataChar->canWrite()) {
        _debugLog("Data characteristic is not writable");
        return false;
    }

    _pTxChar = pSvc->getCharacteristic(SHELLY_BLE_RPC_TX_CTL_CHAR_UUID);
    if (!_pTxChar) {
        _debugLog("TX control characteristic not found (UUID: %s)", SHELLY_BLE_RPC_TX_CTL_CHAR_UUID);
        return false;
    }
    if (!_pTxChar->canWriteNoResponse() && !_pTxChar->canWrite()) {
        _debugLog("TX control characteristic is not writable");
        return false;
    }

    _pRxChar = pSvc->getCharacteristic(SHELLY_BLE_RPC_RX_CTL_CHAR_UUID);
    if (!_pRxChar) {
        _debugLog("RX control characteristic not found (UUID: %s)", SHELLY_BLE_RPC_RX_CTL_CHAR_UUID);
        return false;
    }
    if (!_pRxChar->canRead()) {
        _debugLog("RX control characteristic is not readable");
        return false;
    }

    if (_pRxChar->canNotify() && !_pRxChar->subscribe(true, _staticNotifyCallback)) {
        _debugLog("Warning: failed to subscribe to RX control notifications; falling back to polling");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Internal – fragmented write
// ---------------------------------------------------------------------------

bool ShellyBleRpc::_sendFragmented(const uint8_t* data, size_t length) {
    uint8_t frameLength[4] = {
        static_cast<uint8_t>((length >> 24) & 0xFF),
        static_cast<uint8_t>((length >> 16) & 0xFF),
        static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>(length & 0xFF),
    };

    bool ctlUseResponse = !_pTxChar->canWriteNoResponse();
    if (!_pTxChar->writeValue(frameLength, sizeof(frameLength), ctlUseResponse)) {
        _debugLog("TX control write failed (BLE security / bonding required?)");
        return false;
    }

    // ATT MTU overhead is 3 bytes, so a data write can carry up to MTU - 3
    // bytes of frame payload. Chunking may be arbitrary as long as the total
    // matches the length submitted via the TX control characteristic.
    uint16_t mtu      = _pClient->getMTU();
    size_t   chunkLen = (mtu > 3) ? static_cast<size_t>(mtu - 3) : 20u;

    for (size_t offset = 0; offset < length; offset += chunkLen) {
        size_t thisChunkLen = ((offset + chunkLen) <= length)
                                  ? chunkLen
                                  : (length - offset);
        bool dataUseResponse = !_pDataChar->canWriteNoResponse();
        if (!_pDataChar->writeValue(data + offset, thisChunkLen, dataUseResponse)) {
            _debugLog("Data write failed at offset %u", static_cast<unsigned>(offset));
            return false;
        }

        if (offset + thisChunkLen < length) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return true;
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
    if (length < 4) return;

    uint32_t responseLength =
        (static_cast<uint32_t>(pData[0]) << 24) |
        (static_cast<uint32_t>(pData[1]) << 16) |
        (static_cast<uint32_t>(pData[2]) << 8)  |
        static_cast<uint32_t>(pData[3]);

    _responseLength = responseLength;
    _responseLengthReady = true;
    if (_responseSemaphore) {
        xSemaphoreGive(_responseSemaphore);
    }

    _debugLog("RX notify: response length=%lu", static_cast<unsigned long>(responseLength));
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
    if (!_pDataChar || !_pTxChar || !_pRxChar) {
        _debugLog("Service not set up");
        return false;
    }
    if (timeoutMs == 0) timeoutMs = _timeoutMs;
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

    auto prepareRpcReceiveState = [this]() {
        _responseBuffer = "";
        _responseLength = 0;
        _responseLengthReady = false;
        if (_responseSemaphore) {
            while (xSemaphoreTake(_responseSemaphore, 0) == pdTRUE) {}
        }
    };

    auto trySend = [&]() {
        prepareRpcReceiveState();
        return _sendFragmented(reinterpret_cast<const uint8_t*>(request.c_str()),
                               request.length());
    };

    if (!trySend()) {
        _debugLog("RPC send failed for method %s (write error – ensure the "
                  "device is bonded or does not require BLE authentication)",
                  method);

        bool rpcSent = false;
        for (uint8_t retry = 1; retry < kRpcRetryAttempts; ++retry) {
            bool havePeerAddress = (_pClient != nullptr);
            NimBLEAddress peerAddress;
            if (havePeerAddress) {
                peerAddress = _pClient->getPeerAddress();
            }

            if (!havePeerAddress) {
                _debugLog("No peer address available for RPC retry");
                break;
            }

            uint32_t backoffMs = rpcRetryDelayMs(retry);
            if (backoffMs > 0) {
                delay(backoffMs);
            }

            _debugLog("Retrying RPC (%u/%u) after reconnecting to %s ...",
                      static_cast<unsigned>(retry + 1),
                      static_cast<unsigned>(kRpcRetryAttempts),
                      peerAddress.toString().c_str());

            disconnect();
            if (!connect(peerAddress)) {
                _debugLog("Reconnect failed for RPC retry %u/%u",
                          static_cast<unsigned>(retry + 1),
                          static_cast<unsigned>(kRpcRetryAttempts));
                continue;
            }

            if (trySend()) {
                rpcSent = true;
                break;
            }

            _debugLog("RPC send retry %u/%u failed for method %s",
                      static_cast<unsigned>(retry + 1),
                      static_cast<unsigned>(kRpcRetryAttempts),
                      method);
        }

        if (!rpcSent) {
            return false;
        }
    }

    uint32_t deadline = millis() + timeoutMs;

    if (!_responseSemaphore ||
        xSemaphoreTake(_responseSemaphore, pdMS_TO_TICKS(timeoutMs)) != pdTRUE ||
        !_responseLengthReady || _responseLength == 0) {
        _debugLog("RPC timeout (%lu ms) waiting for response length for method %s",
                  timeoutMs, method);
        return false;
    }

    uint32_t frameLength = _responseLength;

    _debugLog("RPC response length=%lu", static_cast<unsigned long>(frameLength));

    while (_responseBuffer.length() < frameLength &&
           static_cast<int32_t>(millis() - deadline) < 0) {
        std::string chunk = _pDataChar->readValue();
        if (chunk.empty()) {
            delay(kReadEmptyDelayMs);
            continue;
        }

        for (size_t i = 0; i < chunk.length() &&
                           _responseBuffer.length() < SHELLY_BLE_RPC_BUFFER_SIZE; ++i) {
            _responseBuffer += chunk[i];
        }
    }

    if (_responseBuffer.length() == 0) {
        _debugLog("RPC timeout (%lu ms) waiting for response body for method %s",
                  timeoutMs, method);
        return false;
    }

    if (_responseBuffer.length() < frameLength) {
        _debugLog("Short RPC response: expected %lu bytes, got %u",
                  static_cast<unsigned long>(frameLength),
                  static_cast<unsigned>(_responseBuffer.length()));
        response = _responseBuffer;
        return false;
    }

    response = _responseBuffer.substring(0, frameLength);
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
    log_d("%s", buf);
}
