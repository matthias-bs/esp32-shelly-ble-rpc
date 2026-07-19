/**
 * ShellyBleScanWiFiManager.ino
 *
 * Example for the esp32-shelly-ble-rpc library.
 *
 * Combines:
 *   1) WiFiManager-based configuration of a target Shelly device name
 *   2) BLE scan-and-connect using that configured name filter
 *
 * Operation
 * ---------
 * First boot (or when CONFIG_PIN is held LOW at reset):
 *   - Starts a temporary WiFi configuration portal.
 *   - Lets you set an optional exact Shelly advertised device name.
 *   - Saves settings to NVS and restarts.
 *
 * Normal boot:
 *   - WiFi is disabled.
 *   - BLE stack is started.
 *   - Scans for Shelly devices and connects to the strongest match that
 *     satisfies the configured name filter.
 *
 * Notes:
 *   - Leave the name field empty to connect to the strongest nearby Shelly.
 *   - Name match is exact (same behavior as ShellyBleRpc::scan(nameFilter)).
 *   - WiFi and BLE are not used at the same time.
 *
 * Dependencies:
 *   - NimBLE-Arduino
 *   - WiFiManager
 *
 * License: MIT
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ShellyBleRpc.h>

// ============================================================================
// Configuration constants
// ============================================================================

/** GPIO pin that forces config mode when held LOW at boot (BOOT button on many ESP32 boards). */
static const int CONFIG_PIN = 0;

/** WiFiManager AP SSID shown during configuration. */
static const char* CONFIG_AP_SSID = "ShellyBLE-Setup";

/** WiFiManager AP password (minimum 8 characters). */
static const char* CONFIG_AP_PASSWORD = "12345678";

/** Configuration portal timeout in seconds. */
static const uint32_t CONFIG_PORTAL_TIMEOUT_S = 300;

/** Default scan duration in milliseconds. */
static const uint32_t SCAN_DURATION_MS = 5000;

/** Switch component index to toggle in the loop. */
static const uint8_t SWITCH_ID = 0;

/** Delay between loop iterations. */
static const uint32_t LOOP_INTERVAL_MS = 10000;

/** Maximum length for configured device name. */
static const size_t MAX_NAME_FILTER_LEN = 40;

// ============================================================================
// Globals
// ============================================================================

static Preferences prefs;
static ShellyBleRpc shelly;

static String configuredNameFilter;
static bool   hasStoredConfig = false;

// ============================================================================
// NVS helpers
// ============================================================================

static void loadConfig() {
    prefs.begin("shelly-ble", true);
    hasStoredConfig = prefs.getBool("configured", false);
    configuredNameFilter = prefs.getString("name_filter", "");
    prefs.end();

    configuredNameFilter.trim();
}

static void saveConfig(const String& nameFilter) {
    prefs.begin("shelly-ble", false);
    prefs.putBool("configured", true);
    prefs.putString("name_filter", nameFilter);
    prefs.end();
}

// ============================================================================
// WiFiManager configuration
// ============================================================================

static void runConfigPortal() {
    Serial.println("--- WiFiManager configuration mode ---");
    Serial.printf("AP SSID: %s\n", CONFIG_AP_SSID);
    Serial.println("Open the captive portal and set Shelly device name filter.");
    Serial.println("Leave it empty to connect to any Shelly.");

    WiFi.mode(WIFI_AP_STA);

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConfigPortalBlocking(true);
    wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);

    char nameBuf[MAX_NAME_FILTER_LEN + 1] = {0};
    configuredNameFilter.toCharArray(nameBuf, sizeof(nameBuf));

    WiFiManagerParameter pShellyName(
        "shelly_name",
        "Shelly Device Name (exact, optional)",
        nameBuf,
        sizeof(nameBuf)
    );

    wm.addParameter(&pShellyName);

    bool portalOk = wm.startConfigPortal(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);

    String newFilter = String(pShellyName.getValue());
    newFilter.trim();

    if (!portalOk) {
        Serial.println("Config portal timed out or failed. Restarting ...");
        delay(1000);
        ESP.restart();
    }

    saveConfig(newFilter);

    Serial.println("Configuration saved:");
    if (newFilter.length() > 0) {
        Serial.println("  Name filter: " + newFilter);
    } else {
        Serial.println("  Name filter: (none)");
    }

    wm.stopConfigPortal();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

    Serial.println("Restarting ...");
    delay(1000);
    ESP.restart();
}

// ============================================================================
// BLE scan/connect helpers
// ============================================================================

static const char* activeNameFilter() {
    return configuredNameFilter.length() > 0 ? configuredNameFilter.c_str() : nullptr;
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
    if (!shelly.getScanResult(0, bestResult)) {
        Serial.println("No valid scan result entry");
        return false;
    }

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
            bestIndex = i;
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

// ============================================================================
// Setup and loop
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ShellyBleScanWiFiManager example ===");

    pinMode(CONFIG_PIN, INPUT_PULLUP);

    loadConfig();

    bool forceConfig = (digitalRead(CONFIG_PIN) == LOW);
    bool noConfig = !hasStoredConfig;

    // Start configuration on first boot or when forced by button.
    if (forceConfig || noConfig) {
        if (forceConfig) {
            Serial.println("Config button pressed at boot.");
        } else {
            Serial.println("No stored configuration found; entering config mode.");
        }
        runConfigPortal();
    }

    Serial.println("Stored name filter: " + configuredNameFilter);

    // Ensure WiFi is off before starting BLE.
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

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
        Serial.println("Connection lost - rescanning ...");
        ensureConnected();
    }

    String status;
    if (shelly.shellyGetStatus(status)) {
        Serial.println("Shelly status: " + status);
    } else {
        Serial.println("WARNING: Shelly.GetStatus failed");
    }

    Serial.printf("Toggling switch %u ...\n", SWITCH_ID);
    String toggleResult;
    if (shelly.switchToggle(SWITCH_ID, toggleResult)) {
        Serial.println("Toggle result: " + toggleResult);
    } else {
        Serial.println("WARNING: Switch.Toggle failed");
    }

    delay(LOOP_INTERVAL_MS);
}
