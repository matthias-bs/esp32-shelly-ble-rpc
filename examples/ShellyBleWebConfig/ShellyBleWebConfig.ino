/**
 * ShellyBleWebConfig.ino
 *
 * Example for the esp32-shelly-ble-rpc library.
 *
 * Demonstrates how to configure the BLE address of a Shelly device via a
 * temporary WiFi access point and captive web form.  The settings are
 * persisted in non-volatile storage (NVS) via the Preferences library so
 * that the configuration only needs to be entered once.
 *
 * Operation
 * ---------
 *   First boot / no configuration saved:
 *     The ESP32 starts a WiFi AP named "ShellyBLE-Config".  Connect a phone
 *     or laptop to that network, open http://192.168.4.1 and fill in the
 *     BLE address of your Shelly device.  After saving the form the ESP32
 *     restarts and connects to the Shelly device via BLE.
 *
 *   Subsequent boots:
 *     The stored configuration is loaded and the ESP32 connects to the
 *     Shelly device directly without starting the WiFi AP.
 *
 *   Force re-configuration:
 *     During the first RECONFIG_WINDOW_MS after boot, press and hold the
 *     CONFIG_PIN (GPIO0 / BOOT button). The WiFi AP starts again so the
 *     settings can be changed.
 *
 * Hardware requirements
 * ---------------------
 *   - Any ESP32 board (tested with ESP32-DevKitC)
 *   - Shelly Gen2+ device with Bluetooth enabled
 *
 * Library dependencies
 * --------------------
 *   - NimBLE-Arduino  (https://github.com/h2zero/NimBLE-Arduino)
 *   - esp32-shelly-ble-rpc (this library)
 *   - WiFi, WebServer, Preferences (bundled with the ESP32 Arduino core)
 *
 * Note: The WiFi AP and BLE stack are NOT active at the same time.
 *       Config mode uses WiFi only; normal operation uses BLE only.
 *
 * License: MIT
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ShellyBleRpc.h>

// ============================================================================
// Configuration constants – adjust to taste
// ============================================================================

/** SSID of the configuration access point. */
static const char* AP_SSID = "ShellyBLE-Config";

/** Password for the configuration access point (min. 8 characters). */
static const char* AP_PASSWORD = "12345678";

/** IP address assigned to the ESP32 in AP mode. */
static const IPAddress AP_IP(192, 168, 4, 1);

/** How long (ms) to keep the AP open before giving up and rebooting. */
static const uint32_t CONFIG_TIMEOUT_MS = 300000UL;  // 5 minutes

/** GPIO pin used to request reconfiguration during the startup window. */
static const int CONFIG_PIN = 0;

/** Time window after boot in which pressing BOOT enters config mode. */
static const uint32_t RECONFIG_WINDOW_MS = 3000;

/** Switch component index on the Shelly device (0 = first switch). */
static const uint8_t SWITCH_ID = 0;

/** Time between switch operations in the main loop (ms). */
static const uint32_t LOOP_INTERVAL_MS = 10000;

// ============================================================================
// Globals
// ============================================================================

static Preferences prefs;
static WebServer   server(80);
static ShellyBleRpc shelly;

static String  storedBleAddr;
static uint8_t storedAddrType = BLE_ADDR_PUBLIC;

static bool shouldEnterConfigMode() {
    Serial.printf("Press BOOT within %lu ms after startup for web config mode.\n",
                  static_cast<unsigned long>(RECONFIG_WINDOW_MS));

    uint32_t startMs = millis();
    while (millis() - startMs < RECONFIG_WINDOW_MS) {
        if (digitalRead(CONFIG_PIN) == LOW) {
            delay(30);  // Simple debounce for the BOOT button.
            if (digitalRead(CONFIG_PIN) == LOW) {
                return true;
            }
        }
        delay(10);
    }

    return false;
}

// ============================================================================
// Web server pages
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
    body.reserve(512);
    body += "<form action='/save' method='POST'>";
    body += "<label>Shelly BLE Address</label>";
    body += "<input type='text' name='ble_addr'"
            " placeholder='AA:BB:CC:DD:EE:FF'"
            " value='" + storedBleAddr + "'"
            " pattern='[0-9A-Fa-f:]{17}' maxlength='17' required>";
    body += "<p class='note'>Find the BLE address in the Shelly app under"
            " Settings &#x2192; Device Info &#x2192; BLE Address.</p>";
    body += "<label>Address Type</label>";
    body += "<select name='addr_type'>";
    body += "<option value='0'" + String(storedAddrType == 0 ? " selected" : "") +
            ">Public (default)</option>";
    body += "<option value='1'" + String(storedAddrType == 1 ? " selected" : "") +
            ">Random (static)</option>";
    body += "</select>";
    body += "<button class='btn' type='submit'>Save &amp; Restart</button>";
    body += "</form>";
    sendPage(200, body);
}

static void handleSave() {
    if (!server.hasArg("ble_addr")) {
        server.send(400, "text/plain", "Missing ble_addr parameter");
        return;
    }

    String addr = server.arg("ble_addr");
    addr.trim();
    addr.toUpperCase();
    uint8_t addrType = server.hasArg("addr_type")
                           ? static_cast<uint8_t>(server.arg("addr_type").toInt())
                           : 0;

    // Basic sanity check: expect 17 characters in XX:XX:XX:XX:XX:XX format.
    if (addr.length() != 17) {
        String body = "<p style='color:red'>Invalid BLE address format.</p>"
                      "<a href='/'>&#x2190; Back</a>";
        sendPage(400, body);
        return;
    }

    prefs.begin("shelly-ble", false);
    prefs.putString("ble_addr", addr);
    prefs.putUChar("addr_type", addrType);
    prefs.end();

    String body;
    body += "<p style='color:green'>&#x2714; Configuration saved!</p>";
    body += "<p>BLE address: <b>" + addr + "</b></p>";
    body += "<p>The device will restart in a moment ...</p>";
    body += "<meta http-equiv='refresh' content='3;url=/'>";
    sendPage(200, body);

    delay(2000);
    ESP.restart();
}

/** Redirect everything else back to the root form (captive-portal style). */
static void handleNotFound() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "Redirecting ...");
}

// ============================================================================
// Config mode
// ============================================================================

/**
 * Start the WiFi AP, run the web server until the user saves configuration
 * or the timeout elapses, then restart.
 *
 * This function never returns normally; it always ends with ESP.restart().
 */
static void runConfigMode() {
    Serial.println("--- Configuration mode ---");
    Serial.printf("Connect to WiFi:  SSID='%s'  Password='%s'\n",
                  AP_SSID, AP_PASSWORD);
    Serial.printf("Then open:        http://%s\n", AP_IP.toString().c_str());

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    server.on("/",     HTTP_GET,  handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();

    uint32_t startMs = millis();
    while (millis() - startMs < CONFIG_TIMEOUT_MS) {
        server.handleClient();
        delay(2);
    }

    Serial.println("Config timeout reached – restarting");
    ESP.restart();
}

// ============================================================================
// Helpers
// ============================================================================

static void connectToShelly() {
    while (true) {
        Serial.printf("Connecting to Shelly at %s ...\n", storedBleAddr.c_str());
        if (shelly.connect(storedBleAddr.c_str(), storedAddrType)) {
            Serial.println("Connected!");
            return;
        }
        Serial.println("Connection failed – retrying in 5 s");
        delay(5000);
    }
}

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ShellyBleWebConfig example ===");

    pinMode(CONFIG_PIN, INPUT_PULLUP);

    // Load previously saved configuration from NVS.
    prefs.begin("shelly-ble", true);
    storedBleAddr  = prefs.getString("ble_addr", "");
    storedAddrType = prefs.getUChar("addr_type", BLE_ADDR_PUBLIC);
    prefs.end();

    bool forceConfig = shouldEnterConfigMode();
    bool noConfig    = (storedBleAddr.length() == 0);

    if (forceConfig || noConfig) {
        Serial.println(forceConfig ? "BOOT button detected during startup window."
                                   : "No stored configuration found.");
        runConfigMode();   // Does not return
    }

    Serial.printf("Loaded config – BLE address: %s  type: %u\n",
                  storedBleAddr.c_str(), storedAddrType);

    shelly.setDebug(true);

    if (!shelly.begin()) {
        Serial.println("ERROR: Failed to initialise BLE stack");
        while (true) { delay(1000); }
    }

    connectToShelly();

    String info;
    if (shelly.shellyGetDeviceInfo(info)) {
        Serial.println("Device info: " + info);
    }
}

// ============================================================================
// Loop
// ============================================================================

void loop() {
    if (!shelly.isConnected()) {
        Serial.println("Connection lost – reconnecting ...");
        connectToShelly();
    }

    String response;

    if (shelly.switchGet(SWITCH_ID, response)) {
        Serial.println("Switch status: " + response);
    } else {
        Serial.println("WARNING: switchGet failed");
    }

    Serial.println("Toggling switch ...");
    if (shelly.switchToggle(SWITCH_ID, response)) {
        Serial.println("Toggle result: " + response);
    } else {
        Serial.println("WARNING: switchToggle failed");
    }

    delay(LOOP_INTERVAL_MS);
}
