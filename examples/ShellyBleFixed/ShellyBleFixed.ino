/**
 * ShellyBleFixed.ino
 *
 * Example for the esp32-shelly-ble-rpc library.
 *
 * Demonstrates how to connect to a Shelly Gen2+ device using a fixed,
 * hardcoded BLE address.  On every loop iteration the sketch reads the
 * current switch state, prints it, toggles the switch, and waits before
 * repeating.
 *
 * Hardware requirements
 * ---------------------
 *   - Any ESP32 board (tested with ESP32-DevKitC)
 *   - Shelly Gen2+ device with Bluetooth enabled
 *     (Settings → Connectivity → Bluetooth in the Shelly web UI or app)
 *
 * Library dependencies
 * --------------------
 *   - NimBLE-Arduino (https://github.com/h2zero/NimBLE-Arduino)
 *   - esp32-shelly-ble-rpc (this library)
 *
 * How to find the BLE address
 * ---------------------------
 *   Open the Shelly mobile app → select your device → Settings →
 *   Device Info → "BLE Address".  It looks like "AA:BB:CC:DD:EE:FF".
 *
 * License: MIT
 */

#include <Arduino.h>
#include <ShellyBleRpc.h>

// ============================================================================
// User configuration – update these values for your setup
// ============================================================================

/** BLE MAC address of the Shelly device. */
static const char* SHELLY_BLE_ADDR = "AA:BB:CC:DD:EE:FF";

/**
 * BLE address type.
 * Most Shelly devices use a public address (BLE_ADDR_PUBLIC = 0).
 * Some use a random address (BLE_ADDR_RANDOM = 1).
 */
static const uint8_t SHELLY_ADDR_TYPE = BLE_ADDR_PUBLIC;

/** Switch component index (0 = first/only switch). */
static const uint8_t SWITCH_ID = 0;

/** Time between switch operations in milliseconds. */
static const uint32_t LOOP_INTERVAL_MS = 10000;

// ============================================================================

ShellyBleRpc shelly;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Connect (or reconnect) to the Shelly device.  Blocks until successful. */
static void connectToShelly() {
    while (true) {
        Serial.printf("Connecting to Shelly at %s ...\n", SHELLY_BLE_ADDR);
        if (shelly.connect(SHELLY_BLE_ADDR, SHELLY_ADDR_TYPE)) {
            Serial.println("Connected!");
            return;
        }
        Serial.println("Connection failed – retrying in 5 s");
        delay(5000);
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    // Brief pause so the serial monitor can attach before output starts.
    delay(500);
    Serial.println("\n=== ShellyBleFixed example ===");

    shelly.setDebug(true);   // Remove or set false to suppress verbose output

    if (!shelly.begin()) {
        Serial.println("ERROR: Failed to initialise BLE stack");
        while (true) { delay(1000); }
    }

    connectToShelly();

    // Print basic device information once after connecting.
    String info;
    if (shelly.shellyGetDeviceInfo(info)) {
        Serial.println("Device info: " + info);
    } else {
        Serial.println("WARNING: Could not read device info");
    }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void loop() {
    // Re-connect automatically if the BLE link drops.
    if (!shelly.isConnected()) {
        Serial.println("Connection lost – reconnecting ...");
        connectToShelly();
    }

    String response;

    // Read the current switch state.
    if (shelly.switchGet(SWITCH_ID, response)) {
        Serial.println("Switch status: " + response);
    } else {
        Serial.println("WARNING: switchGet failed");
    }

    // Toggle the switch.
    Serial.println("Toggling switch ...");
    if (shelly.switchToggle(SWITCH_ID, response)) {
        Serial.println("Toggle result: " + response);
    } else {
        Serial.println("WARNING: switchToggle failed");
    }

    delay(LOOP_INTERVAL_MS);
}
