/**
 * ShellyBleScanWiFiManager.ino
 *
 * Example for the esp32-shelly-ble-rpc library.
 *
 * Combines:
 *   1) WebServer-based configuration of a target Shelly device name
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
 *   - Name match is exact and case-sensitive (same behavior as
 *     ShellyBleRpc::scan(nameFilter)).
 *   - WiFi and BLE are not used at the same time.
 *
 * Dependencies:
 *   - NimBLE-Arduino
 *   - WiFi, WebServer, Preferences (bundled with ESP32 Arduino core)
 *
 * License: MIT
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ShellyBleRpc.h>

// ============================================================================
// Configuration constants
// ============================================================================

/** GPIO pin that forces config mode when held LOW at boot (BOOT button on many ESP32 boards). */
static const int CONFIG_PIN = 0;

/** AP SSID shown during configuration. */
static const char* CONFIG_AP_SSID = "ShellyBLE-Setup";

/** AP password (minimum 8 characters). */
static const char* CONFIG_AP_PASSWORD = "12345678";

/** Configuration portal timeout in seconds. */
static const uint32_t CONFIG_PORTAL_TIMEOUT_S = 300;

/** IP address assigned to the ESP32 in AP mode. */
static const IPAddress CONFIG_AP_IP(192, 168, 4, 1);

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
static WebServer   server(80);
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
// Web configuration portal
// ============================================================================

// Minimal CSS / HTML shared across pages.
static const char HTML_STYLE[] PROGMEM =
    "body{font-family:Arial,sans-serif;max-width:480px;margin:40px auto;"
    "padding:20px;background:#f4f4f4;}"
    "h1{color:#d63031;}"
    ".card{background:#fff;padding:20px;border-radius:8px;"
    "box-shadow:0 2px 6px rgba(0,0,0,.15);}"
    "label{display:block;margin-top:14px;font-weight:bold;}"
    "input,select{width:100%;padding:8px;margin-top:4px;box-sizing:border-box;"
    "border:1px solid #ccc;border-radius:4px;font-size:1em;}"
    ".btn{display:block;margin-top:22px;width:100%;padding:12px;background:#d63031;"
    "color:#fff;border:none;border-radius:4px;font-size:1em;cursor:pointer;}"
    ".btn:hover{background:#c0392b;}"
    ".note{color:#636e72;font-size:.85em;margin-top:6px;}";

static void sendPage(int code, const String& body) {
    String page;
    page.reserve(800 + body.length());
    page += "<!DOCTYPE html><html><head>"
            "<meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Shelly BLE Config</title>"
            "<style>";
    page += FPSTR(HTML_STYLE);
    page += "</style></head><body><div class='card'>"
            "<h1>&#x1F4F6; Shelly BLE Config</h1>";
    page += body;
    page += "</div></body></html>";
    server.send(code, "text/html", page);
}

static void handleRoot() {
    String body;
    body.reserve(600);
    body += "<form action='/save' method='POST'>";
    body += "<label>Shelly Device Name (exact, case-sensitive, optional)</label>";
    body += "<input type='text' name='name_filter'"
            " placeholder='ShellyPlus1-ABCDEF'"
            " value='" + configuredNameFilter + "'"
            " maxlength='" + String(MAX_NAME_FILTER_LEN) + "'>";
    body += "<p class='note'>Leave empty to connect to the strongest nearby Shelly.</p>";
    body += "<button class='btn' type='submit'>Save &amp; Restart</button>";
    body += "</form>";
    sendPage(200, body);
}

static void handleSave() {
    String newFilter = server.hasArg("name_filter") ? server.arg("name_filter") : "";
    newFilter.trim();

    if (newFilter.length() > MAX_NAME_FILTER_LEN) {
        String body = "<p style='color:red'>Name is too long.</p>"
                      "<a href='/'>&#x2190; Back</a>";
        sendPage(400, body);
        return;
    }

    saveConfig(newFilter);

    String body;
    body += "<p style='color:green'>&#x2714; Configuration saved!</p>";
    if (newFilter.length() > 0) {
        body += "<p>Name filter: <b>" + newFilter + "</b></p>";
    } else {
        body += "<p>Name filter: <b>(none)</b></p>";
    }
    body += "<p>The device will restart in a moment ...</p>";
    sendPage(200, body);

    delay(1500);
    ESP.restart();
}

/** Redirect everything else back to the root form (captive-portal style). */
static void handleNotFound() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Redirecting ...");
}

static void runConfigPortal() {
    Serial.println("--- Configuration mode ---");
    Serial.printf("AP SSID: %s\n", CONFIG_AP_SSID);
    Serial.printf("AP IP:   %s\n", CONFIG_AP_IP.toString().c_str());
    Serial.println("Open http://192.168.4.1 and set Shelly device name filter.");
    Serial.println("Leave it empty to connect to any nearby Shelly.");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(CONFIG_AP_IP, CONFIG_AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();

    uint32_t startMs = millis();
    while (millis() - startMs < (CONFIG_PORTAL_TIMEOUT_S * 1000UL)) {
        server.handleClient();
        delay(2);
    }

    Serial.println("Config timeout reached. Restarting ...");

    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

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
