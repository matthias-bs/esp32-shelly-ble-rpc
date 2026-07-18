/**
 * ShellyBleScanConnect.ino
 *
 * Example for the esp32-shelly-ble-rpc library.
 *
 * Demonstrates the scan-and-connect flow: the ESP32 scans for nearby Shelly
 * Gen2+ devices that advertise the mOS BLE RPC service, prints the matching
 * results, connects to the strongest match, then reads generic Shelly RPC
 * data that is available on any supported device class.
 *
 * Optional: set SHELLY_NAME_FILTER to the exact advertised device name to
 * target one specific Shelly. Leave it empty to connect to the strongest
 * nearby Shelly device.
 *
 * License: MIT
 */

#include <Arduino.h>
#include <ShellyBleRpc.h>

// ============================================================================
// User configuration
// ============================================================================

/** Exact advertised device name, e.g. "shellyplus1-123456", or "" for any. */
static const char* SHELLY_NAME_FILTER = "";

/** Scan time in milliseconds. */
static const uint32_t SCAN_DURATION_MS = 5000;

/** Switch component index to toggle on every loop iteration (0-based). */
static const uint8_t SWITCH_ID = 0;

/** Time between status reads in milliseconds. */
static const uint32_t LOOP_INTERVAL_MS = 10000;

// ============================================================================

ShellyBleRpc shelly;

static const char* activeNameFilter() {
    return SHELLY_NAME_FILTER[0] != '\0' ? SHELLY_NAME_FILTER : nullptr;
}

static bool scanAndConnectToShelly() {
    const char* nameFilter = activeNameFilter();

    if (nameFilter) {
        Serial.printf("Scanning for Shelly '%s' ...\n", nameFilter);
    } else {
        Serial.println("Scanning for nearby Shelly devices ...");
    }

    if (!shelly.scan(SCAN_DURATION_MS, nameFilter)) {
        Serial.println("No matching Shelly devices found");
        return false;
    }

    size_t bestIndex = 0;
    ShellyBleRpc::ScanResult bestResult;
    shelly.getScanResult(0, bestResult);

    for (size_t i = 0; i < shelly.getScanResultCount(); ++i) {
        ShellyBleRpc::ScanResult result;
        if (!shelly.getScanResult(i, result)) {
            continue;
        }

        Serial.printf("  [%u] %s  RSSI=%d  name=%s  type=%u\n",
                      static_cast<unsigned>(i),
                      result.address.c_str(),
                      result.rssi,
                      result.name.length() ? result.name.c_str() : "(none)",
                      result.addressType);

        if (result.rssi > bestResult.rssi) {
            bestIndex  = i;
            bestResult = result;
        }
    }

    Serial.printf("Connecting to %s ...\n", bestResult.address.c_str());
    if (!shelly.connectToScanResult(bestIndex)) {
        Serial.println("Connection failed");
        return false;
    }

    Serial.println("Connected!");
    return true;
}

static void ensureConnected() {
    while (!scanAndConnectToShelly()) {
        Serial.println("Retrying in 5 s ...");
        delay(5000);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ShellyBleScanConnect example ===");

    shelly.setDebug(true);

    if (!shelly.begin()) {
        Serial.println("ERROR: Failed to initialise BLE stack");
        while (true) { delay(1000); }
    }

    ensureConnected();

    String info;
    if (shelly.shellyGetDeviceInfo(info)) {
        Serial.println("Device info: " + info);
    } else {
        Serial.println("WARNING: Could not read device info");
    }
}

void loop() {
    if (!shelly.isConnected()) {
        Serial.println("Connection lost – rescanning ...");
        ensureConnected();
    }

    String status;
    if (shelly.shellyGetStatus(status)) {
        Serial.println("Shelly status: " + status);
    } else {
        Serial.println("WARNING: Shelly.GetStatus failed");
    }

    // Toggle switch after reading status.
    Serial.printf("Toggling switch %u ...\n", SWITCH_ID);
    String toggleResult;
    if (shelly.switchToggle(SWITCH_ID, toggleResult)) {
        Serial.println("Toggle result: " + toggleResult);
    } else {
        Serial.println("WARNING: Switch.Toggle failed");
    }

    delay(LOOP_INTERVAL_MS);
}
