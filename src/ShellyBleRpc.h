/**
 * @file ShellyBleRpc.h
 * @brief Arduino library for controlling Shelly Gen2+ devices via BLE RPC from an ESP32.
 *
 * Uses NimBLE-Arduino for BLE communication and implements the mOS (mongoose-os)
 * BLE RPC protocol used by all Shelly Gen2+ devices.
 *
 * BLE RPC Protocol overview:
 *   - A GATT service with three characteristics is used: TX control (client → device)
 *     for the request length, data (client ↔ device) for request/response bytes,
 *     and RX control (device → client) for the response length (read/notify).
 *   - Data is written in MTU-sized chunks without a per-chunk header; the total
 *     frame length is sent via TX control and response length via RX control.
 *   - The payload is a JSON-RPC 2.0 object, e.g.
 *       {"id":1,"method":"Switch.Set","params":{"id":0,"on":true}}
 *
 * Requires: NimBLE-Arduino (https://github.com/h2zero/NimBLE-Arduino)
 *
 * License: MIT
 */

#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>

// ---------------------------------------------------------------------------
// Shelly / mOS BLE RPC UUIDs
//
// These are the standard mOS (mongoose-os) RPC-over-GATT UUIDs used by
// Shelly Gen2+ devices.  Each UUID encodes the corresponding ASCII string:
//   Service  "_mOS_RPC_SVC_ID_"  → 5F6D4F53-5F52-5043-5F53-56435F49445F
//   Data     "_mOS_RPC_data___"  → 5F6D4F53-5F52-5043-5F64-6174615F5F5F
//   TX ctl   "_mOS_RPC_tx_ctl_"  → 5F6D4F53-5F52-5043-5F74-785F63746C5F
//   RX ctl   "_mOS_RPC_rx_ctl_"  → 5F6D4F53-5F52-5043-5F72-785F63746C5F
// ---------------------------------------------------------------------------
#define SHELLY_BLE_RPC_SERVICE_UUID  "5F6D4F53-5F52-5043-5F53-56435F49445F"
/** Read/write data attribute used to send request bytes and read response bytes. */
#define SHELLY_BLE_RPC_DATA_CHAR_UUID    "5F6D4F53-5F52-5043-5F64-6174615F5F5F"
/** Write-only control attribute used to submit request frame length. */
#define SHELLY_BLE_RPC_TX_CTL_CHAR_UUID  "5F6D4F53-5F52-5043-5F74-785F63746C5F"
/** Read/notify control attribute used to obtain response frame length. */
#define SHELLY_BLE_RPC_RX_CTL_CHAR_UUID  "5F6D4F53-5F52-5043-5F72-785F63746C5F"

/** Default timeout for RPC calls in milliseconds. */
#define SHELLY_BLE_RPC_DEFAULT_TIMEOUT_MS  10000U
/** Default scan duration in milliseconds. */
#define SHELLY_BLE_RPC_DEFAULT_SCAN_MS     5000U
/** Maximum accumulated response size (bytes) before truncation. */
#define SHELLY_BLE_RPC_BUFFER_SIZE         4096U

// ---------------------------------------------------------------------------

/**
 * @brief Controls a Shelly Gen2+ device via BLE JSON-RPC.
 *
 * Only one instance should exist at a time (the static notify callback holds
 * a pointer to the most recently constructed instance).
 *
 * Example – basic usage:
 * @code
 *   ShellyBleRpc shelly;
 *   shelly.setDebug(true);
 *   shelly.begin();
 *   if (shelly.connect("AA:BB:CC:DD:EE:FF")) {
 *       String info;
 *       if (shelly.shellyGetDeviceInfo(info)) {
 *           Serial.println(info);
 *       }
 *       String res;
 *       shelly.switchSet(0, true, res);
 *       shelly.disconnect();
 *   }
 * @endcode
 */
class ShellyBleRpc {
public:
    struct ScanResult {
        String  address;      ///< BLE address, e.g. "AA:BB:CC:DD:EE:FF"
        String  name;         ///< Advertised device name, may be empty
        int     rssi;         ///< RSSI reported during the scan
        uint8_t addressType;  ///< BLE_ADDR_PUBLIC or BLE_ADDR_RANDOM
    };

    ShellyBleRpc();
    ~ShellyBleRpc();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Initialise the NimBLE stack.
     *
     * Must be called once before any connect() call.  Do not call when a
     * WiFi AP / station interface is already active – power up one radio at a
     * time on the ESP32.
     *
     * @param deviceName Advertised BLE device name for this ESP32.
     * @return true on success.
     */
    bool begin(const char* deviceName = "ShellyBleRpc");

    /**
     * @brief Connect to a Shelly device by BLE address string.
     *
     * @param address  MAC address string, e.g. "AA:BB:CC:DD:EE:FF".
     * @param addressType BLE_ADDR_PUBLIC (0) or BLE_ADDR_RANDOM (1).
     * @return true if the connection was established and the RPC service found.
     */
    bool connect(const char* address, uint8_t addressType = BLE_ADDR_PUBLIC);

    /**
     * @brief Connect to a Shelly device using a NimBLEAddress object.
     * @return true if the connection was established and the RPC service found.
     */
    bool connect(const NimBLEAddress& address);

    /**
     * @brief Scan for nearby Shelly devices that advertise the RPC service.
     *
     * A device is considered a match when it either advertises the Shelly RPC
     * service UUID or has an advertised name starting with "Shelly" (case-
     * insensitive).  The second criterion handles Shelly devices that omit the
     * service UUID from their advertisement packets (e.g. when in pairing
     * mode).
     *
     * Results can be read with getScanResultCount() / getScanResult() and
     * connected to with connectToScanResult().
     *
     * @param durationMs Scan duration in milliseconds.
     * @param nameFilter Optional exact advertised-name filter, or nullptr.
     * @return true if at least one matching device was found.
     */
    bool scan(uint32_t durationMs = SHELLY_BLE_RPC_DEFAULT_SCAN_MS,
              const char* nameFilter = nullptr);

    /** @return Number of matching devices from the last scan. */
    size_t getScanResultCount() const;

    /**
     * @brief Copy one scan result from the last scan.
     * @param index Result index from 0 to getScanResultCount() - 1.
     * @param result Output scan result.
     * @return true if @p index was valid.
     */
    bool getScanResult(size_t index, ScanResult& result) const;

    /**
     * @brief Connect using a result from the most recent scan.
     * @param index Result index from the last scan.
     * @return true if the connection was established and the RPC service found.
     */
    bool connectToScanResult(size_t index);

    /**
     * @brief Scan and connect to the strongest matching Shelly device.
     * @param durationMs Scan duration in milliseconds.
     * @param nameFilter Optional exact advertised-name filter, or nullptr.
     * @return true if a matching device was found and connected.
     */
    bool scanAndConnect(uint32_t durationMs = SHELLY_BLE_RPC_DEFAULT_SCAN_MS,
                        const char* nameFilter = nullptr);

    /** @brief Disconnect from the Shelly device. */
    void disconnect();

    /** @return true if currently connected to a Shelly device. */
    bool isConnected() const;

    // -----------------------------------------------------------------------
    // Generic RPC
    // -----------------------------------------------------------------------

    /**
     * @brief Perform a JSON-RPC 2.0 call and wait for the response.
     *
     * Sends:  {"id":<n>,"method":"<method>","params":<params>}
     * On success, @p response contains the full JSON response string, which
     * may be either a result object {"id":<n>,"result":{...}} or an error
     * object {"id":<n>,"error":{"code":<c>,"message":"<m>"}}.  The caller
     * must inspect the returned JSON to distinguish the two cases.
     *
     * This function blocks the calling task until the response arrives or the
     * timeout elapses.  It is not safe to call concurrently from multiple
     * tasks.
     *
     * @param method    RPC method name, e.g. "Switch.Set".
     * @param params    JSON parameter string, e.g. "{\"id\":0,\"on\":true}".
     *                  Pass nullptr or "{}" for methods with no parameters.
     * @param response  Output: JSON response string.
     * @param timeoutMs Timeout in milliseconds. Pass 0 to use the value set via setTimeout().
     * @return true if a response was received within the timeout.
     */
    bool call(const char* method, const char* params, String& response,
              uint32_t timeoutMs = 0);

    // -----------------------------------------------------------------------
    // Shelly component convenience methods
    // -----------------------------------------------------------------------

    /** @brief Shelly.GetDeviceInfo – device model, MAC, firmware, etc. */
    bool shellyGetDeviceInfo(String& response);

    /** @brief Shelly.GetStatus – aggregated status of all components. */
    bool shellyGetStatus(String& response);

    /** @brief Shelly.GetConfig – complete device configuration. */
    bool shellyGetConfig(String& response);

    /** @brief Shelly.Reboot – schedule a reboot (500 ms delay). */
    bool shellyReboot(String& response);

    /**
     * @brief Switch.Get – read the current state of a switch component.
     * @param id Component index (0-based).
     */
    bool switchGet(uint8_t id, String& response);

    /**
     * @brief Switch.Set – turn a switch on or off.
     * @param id    Component index.
     * @param state true = on, false = off.
     */
    bool switchSet(uint8_t id, bool state, String& response);

    /**
     * @brief Switch.Toggle – toggle a switch.
     * @param id Component index.
     */
    bool switchToggle(uint8_t id, String& response);

    /**
     * @brief Input.Get – read the state of a digital input.
     * @param id Component index.
     */
    bool inputGet(uint8_t id, String& response);

    /**
     * @brief Temperature.Get – read a temperature sensor value.
     * @param id Component index.
     */
    bool temperatureGet(uint8_t id, String& response);

    /**
     * @brief Humidity.Get – read a relative-humidity sensor value.
     * @param id Component index.
     */
    bool humidityGet(uint8_t id, String& response);

    /**
     * @brief Cover.Open – start opening a roller/cover.
     * @param id Component index.
     */
    bool coverOpen(uint8_t id, String& response);

    /**
     * @brief Cover.Close – start closing a roller/cover.
     * @param id Component index.
     */
    bool coverClose(uint8_t id, String& response);

    /**
     * @brief Cover.Stop – stop a roller/cover.
     * @param id Component index.
     */
    bool coverStop(uint8_t id, String& response);

    /**
     * @brief Cover.GoToPosition – move a roller/cover to a target position.
     * @param id  Component index.
     * @param pos Target position in percent (0 = fully closed, 100 = fully open).
     */
    bool coverGoToPosition(uint8_t id, uint8_t pos, String& response);

    // -----------------------------------------------------------------------
    // Configuration helpers
    // -----------------------------------------------------------------------

    /** @brief Enable / disable verbose debug output on Serial. */
    void setDebug(bool enable);

    /**
     * @brief Override the default RPC call timeout.
     * @param timeoutMs New timeout in milliseconds.
     */
    void setTimeout(uint32_t timeoutMs);

    /**
     * @brief Return the negotiated ATT MTU with the peer device.
     *
     * NimBLE requests up to 517 bytes; the actual value depends on the peer.
     * A larger MTU means fewer BLE fragments per RPC message.
     */
    uint16_t getMTU() const;

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** @brief Discover the RPC service and its data / control characteristics. */
    bool _setupService();

    /**
     * @brief Send one RPC frame using the Shelly BLE RPC transport.
     *
     * First writes the frame length (big-endian uint32) to the TX control
     * characteristic, then writes the frame bytes to the data characteristic
     * in BLE-MTU-sized chunks.
     *
     * @return true if all writes succeeded; false on the first write error.
     */
    bool _sendFragmented(const uint8_t* data, size_t length);

    /** @brief Handle an RX-control notification carrying the response length. */
    void _handleNotification(const uint8_t* pData, size_t length);

    /** @brief Print a debug message prefixed with "[ShellyBleRpc] ". */
    void _debugLog(const char* fmt, ...);

    // -----------------------------------------------------------------------
    // Static NimBLE callback (NimBLE requires a plain function pointer)
    // -----------------------------------------------------------------------
    static void _staticNotifyCallback(NimBLERemoteCharacteristic* pChar,
                                      uint8_t* pData, size_t length,
                                      bool isNotify);

    /** Pointer to the active ShellyBleRpc instance (used by static callback). */
    static ShellyBleRpc* _instance;

    // -----------------------------------------------------------------------
    // NimBLE client event callbacks
    // -----------------------------------------------------------------------
    class ClientCallbacks : public NimBLEClientCallbacks {
    public:
        explicit ClientCallbacks(ShellyBleRpc* parent) : _parent(parent) {}
        void onConnect(NimBLEClient* pClient) override;
        void onDisconnect(NimBLEClient* pClient, int reason) override;
        void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
    private:
        ShellyBleRpc* _parent;
    };

    // -----------------------------------------------------------------------
    // Member variables
    // -----------------------------------------------------------------------
    NimBLEClient*               _pClient;
    NimBLERemoteCharacteristic* _pDataChar;    ///< Data char (request/response bytes)
    NimBLERemoteCharacteristic* _pTxChar;      ///< TX control char (request length)
    NimBLERemoteCharacteristic* _pRxChar;      ///< RX control char (response length)
    ClientCallbacks*            _pCallbacks;

    bool      _debug;
    uint32_t  _timeoutMs;
    bool      _initDone;
    int       _rpcId;

    String               _responseBuffer;      ///< Accumulated response bytes
    SemaphoreHandle_t    _responseSemaphore;   ///< Reserved for async receive use
    std::vector<ScanResult> _scanResults;      ///< Matching devices from the last scan
};
