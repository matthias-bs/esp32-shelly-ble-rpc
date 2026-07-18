# esp32-shelly-ble-rpc

Arduino library for controlling **Shelly Gen2+** devices via **JSON-RPC over BLE** from an **ESP32**, using [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino).

---

## Features

- Connect to any Shelly Gen2+ device by BLE address
- Scan for nearby Shelly Gen2+ devices and connect to the strongest match
- Send arbitrary JSON-RPC 2.0 requests and receive responses
- Convenience wrappers for the most common Shelly components:
  - **Switch** – get / set / toggle
  - **Input** – get state
  - **Temperature / Humidity** – read sensor values
  - **Cover / Roller** – open / close / stop / go-to-position
  - **Shelly** – device info, status, config, reboot
- Automatic BLE MTU negotiation (up to 517 bytes) for efficient data transfer
- FreeRTOS-semaphore-based synchronisation (no busy-wait polling)
- Debug output toggle
- Three ready-to-use examples

---

## Requirements

| Component | Notes |
|-----------|-------|
| ESP32 board | Any variant with Bluetooth (ESP32, ESP32-S3, etc.) |
| Arduino core for ESP32 | ≥ 2.x recommended |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | ≥ 2.0.0 |
| Shelly device | Gen2 or later with BLE enabled |

> **Note:** Do **not** run the WiFi AP (config mode) and BLE (normal mode) at
> the same time.  The examples handle this by using only one radio at a time
> and rebooting between modes.

---

## Installation

### Arduino IDE (Library Manager)

1. Open **Sketch → Include Library → Manage Libraries …**
2. Search for **esp32-shelly-ble-rpc** and install.
3. Also install **NimBLE-Arduino** if not already present.

### Manual / PlatformIO

Clone or download this repository and place it in your Arduino libraries
folder (usually `~/Documents/Arduino/libraries/`), or add it to your
`platformio.ini`:

```ini
lib_deps =
    https://github.com/matthias-bs/esp32-shelly-ble-rpc.git
    h2zero/NimBLE-Arduino
```

---

## Shelly device setup

Before the ESP32 can communicate with a Shelly device over BLE:

1. Open the **Shelly mobile app** or the **Shelly web UI**.
2. Navigate to **Settings → Connectivity → Bluetooth** and enable it.
3. Note the **BLE Address** shown in **Settings → Device Info**
   (format `AA:BB:CC:DD:EE:FF`).  You will need this address in your sketch.

### BLE pairing / bonding (first-time setup)

Some Shelly firmware versions and configurations require the ESP32 to be
**bonded** (paired) with the Shelly device before allowing GATT writes.  If
you see `writeValue failed` errors accompanied by `secureConnection failed`
in the debug output, you need to complete the one-time BLE pairing:

1. In the Shelly app or web UI go to
   **Settings → Connectivity → Bluetooth → Enable Bluetooth pairing**.
2. While pairing mode is active, run your ESP32 sketch.  The library calls
   `secureConnection()` automatically; NimBLE will perform a "Just Works"
   bond (no passkey required) and store the keys in the ESP32's NVS flash.
3. After the first successful connection the bond is persisted.
   Pairing mode on the Shelly can be disabled again – subsequent connections
   use the stored Long-Term Key to re-encrypt without re-pairing.

---

## Quick start

```cpp
#include <ShellyBleRpc.h>

ShellyBleRpc shelly;

void setup() {
    Serial.begin(115200);
    shelly.setDebug(true);
    shelly.begin();

    if (!shelly.connect("AA:BB:CC:DD:EE:FF")) {
        Serial.println("Connection failed");
        return;
    }

    String info;
    if (shelly.shellyGetDeviceInfo(info)) {
        Serial.println(info);          // JSON response
    }

    String res;
    shelly.switchSet(0, true, res);    // Turn switch 0 on
}

void loop() {}
```

---

## API reference

### Lifecycle

| Method | Description |
|--------|-------------|
| `bool begin(const char* deviceName = "ShellyBleRpc")` | Initialise the NimBLE stack.  Call once in `setup()`. |
| `bool connect(const char* address, uint8_t addressType = BLE_ADDR_PUBLIC)` | Connect by MAC address string. |
| `bool connect(const NimBLEAddress& address)` | Connect by `NimBLEAddress` object. |
| `bool scan(uint32_t durationMs = SHELLY_BLE_RPC_DEFAULT_SCAN_MS, const char* nameFilter = nullptr)` | Scan for nearby Shelly devices advertising the RPC service. |
| `size_t getScanResultCount() const` | Return the number of matching devices from the last scan. |
| `bool getScanResult(size_t index, ScanResult& result) const` | Copy one scan result from the last scan. |
| `bool connectToScanResult(size_t index)` | Connect to a device from the last scan result set. |
| `bool scanAndConnect(uint32_t durationMs = SHELLY_BLE_RPC_DEFAULT_SCAN_MS, const char* nameFilter = nullptr)` | Scan and connect to the strongest matching Shelly device. |
| `void disconnect()` | Disconnect from the device. |
| `bool isConnected() const` | `true` if a BLE connection is active. |

### Generic RPC

```cpp
bool call(const char* method, const char* params,
          String& response,
          uint32_t timeoutMs = 0);
```

Sends `{"id":<n>,"method":"<method>","params":<params>}` and waits for
the JSON response.  Returns `true` when a response was received in time.
Both success (`"result"`) and error (`"error"`) responses count as
received; parse the returned JSON to distinguish them.
Pass `timeoutMs = 0` (the default) to use the timeout configured via `setTimeout()`.

### Convenience methods

#### Device

| Method | RPC call |
|--------|----------|
| `shellyGetDeviceInfo(response)` | `Shelly.GetDeviceInfo` |
| `shellyGetStatus(response)` | `Shelly.GetStatus` |
| `shellyGetConfig(response)` | `Shelly.GetConfig` |
| `shellyReboot(response)` | `Shelly.Reboot` (500 ms delay) |

#### Switch

| Method | RPC call |
|--------|----------|
| `switchGet(id, response)` | `Switch.Get` |
| `switchSet(id, state, response)` | `Switch.Set` |
| `switchToggle(id, response)` | `Switch.Toggle` |

#### Input

| Method | RPC call |
|--------|----------|
| `inputGet(id, response)` | `Input.Get` |

#### Temperature / Humidity

| Method | RPC call |
|--------|----------|
| `temperatureGet(id, response)` | `Temperature.Get` |
| `humidityGet(id, response)` | `Humidity.Get` |

#### Cover / Roller

| Method | RPC call |
|--------|----------|
| `coverOpen(id, response)` | `Cover.Open` |
| `coverClose(id, response)` | `Cover.Close` |
| `coverStop(id, response)` | `Cover.Stop` |
| `coverGoToPosition(id, pos, response)` | `Cover.GoToPosition` (`pos` 0–100 %) |

### Configuration

| Method | Description |
|--------|-------------|
| `void setDebug(bool enable)` | Enable/disable verbose `[ShellyBleRpc]` output on `Serial`. |
| `void setTimeout(uint32_t ms)` | Override the default RPC timeout. |
| `uint16_t getMTU() const` | Return the negotiated ATT MTU with the peer. |

### Constants

| Constant | Default | Description |
|----------|---------|-------------|
| `SHELLY_BLE_RPC_SERVICE_UUID` | `5F6D4F53-5F52-5043-5F53-56435F49445F` | mOS RPC GATT service UUID |
| `SHELLY_BLE_RPC_DATA_CHAR_UUID` | `5F6D4F53-5F52-5043-5F64-6174615F5F5F` | Read/write data characteristic for request and response bytes |
| `SHELLY_BLE_RPC_TX_CTL_CHAR_UUID` | `5F6D4F53-5F52-5043-5F74-785F63746C5F` | Write-only request-length control characteristic |
| `SHELLY_BLE_RPC_RX_CTL_CHAR_UUID` | `5F6D4F53-5F52-5043-5F72-785F63746C5F` | Read/notify response-length control characteristic |
| `SHELLY_BLE_RPC_DEFAULT_TIMEOUT_MS` | `10000` | Default RPC timeout (ms) |
| `SHELLY_BLE_RPC_DEFAULT_SCAN_MS` | `5000` | Default BLE scan duration (ms) |
| `SHELLY_BLE_RPC_BUFFER_SIZE` | `4096` | Max accumulated response size (bytes) |

---

## Examples

### ShellyBleFixed

[`examples/ShellyBleFixed/ShellyBleFixed.ino`](examples/ShellyBleFixed/ShellyBleFixed.ino)

Connects to a Shelly device whose BLE address is hardcoded in the sketch.
Reads device info once on startup, then repeatedly reads the switch state,
toggles it, and waits.

**Configure** by editing the constants at the top of the sketch:

```cpp
static const char*   SHELLY_BLE_ADDR  = "AA:BB:CC:DD:EE:FF";
static const uint8_t SHELLY_ADDR_TYPE = BLE_ADDR_PUBLIC;
static const uint8_t SWITCH_ID        = 0;
```

### ShellyBleScanConnect

[`examples/ShellyBleScanConnect/ShellyBleScanConnect.ino`](examples/ShellyBleScanConnect/ShellyBleScanConnect.ino)

Scans for nearby Shelly devices advertising the BLE RPC service, prints the
matching results, and connects to the strongest match. Optionally filters by
the exact advertised device name, then reads generic Shelly RPC data that is
available on any supported device class.

**Configure** by editing the constants at the top of the sketch:

```cpp
static const char* SHELLY_NAME_FILTER = "";
static const uint32_t SCAN_DURATION_MS = 5000;
```

### ShellyBleWiFiConfig

[`examples/ShellyBleWiFiConfig/ShellyBleWiFiConfig.ino`](examples/ShellyBleWiFiConfig/ShellyBleWiFiConfig.ino)

On the **first boot** (or when the BOOT button is held at reset) the ESP32
starts a WiFi access point `ShellyBLE-Config` and hosts a simple web form
at `http://192.168.4.1`.  Enter the Shelly BLE address and save; the device
restarts and connects to Shelly automatically from then on.

| Step | Action |
|------|--------|
| 1 | Connect laptop/phone to `ShellyBLE-Config` (password `12345678`) |
| 2 | Open `http://192.168.4.1` in a browser |
| 3 | Enter the BLE address and press **Save & Restart** |
| 4 | On restart the ESP32 connects to the Shelly via BLE |
| Re-configure | Hold GPIO0 LOW while pressing RESET |

---

## BLE RPC protocol details

The library implements the **mOS (mongoose-os) BLE RPC** transport over GATT:

1. **Service UUID** `5F6D4F53-5F52-5043-5F53-56435F49445F` ("_mOS_RPC_SVC_ID_")
2. **TX control characteristic** (Write/Write-Without-Response) – the client
   writes a 4-byte big-endian unsigned integer representing the total request
   frame length before sending any payload bytes.
3. **Data characteristic** (Read/Write/Write-Without-Response) – the client
   writes the request payload in BLE-MTU-sized chunks (no per-chunk header);
   the server assembles them into the complete frame using the length from step 2.
4. **RX control characteristic** (Read/Notify) – the device notifies the client
   with a 4-byte big-endian unsigned integer representing the total response
   frame length once the full response is ready.  The client waits for this
   notification via a FreeRTOS semaphore.
5. **Data characteristic (read)** – after receiving the RX control notification
   the client reads the response payload from the data characteristic in chunks
   until `frameLength` bytes have been accumulated.

```
Request:
  TX_CTL write: [ len_3 | len_2 | len_1 | len_0 ]   (4-byte big-endian frame length)
  DATA write(s): [ payload chunk 0 ] [ payload chunk 1 ] …

Response:
  RX_CTL notify: [ len_3 | len_2 | len_1 | len_0 ]  (4-byte big-endian frame length)
  DATA read(s):  [ response chunk 0 ] [ response chunk 1 ] …
```

The payload is a complete JSON-RPC 2.0 object, e.g.
`{"id":1,"method":"Switch.Set","params":{"id":0,"on":true}}`.

---

## Limitations / known issues

- **Single instance**: Only one `ShellyBleRpc` object should be active at a
  time.  The static notify callback always forwards to the most recently
  constructed instance.
- **No concurrent calls**: `call()` is not thread-safe; do not invoke it from
  multiple FreeRTOS tasks simultaneously.
- **WiFi + BLE coexistence**: While ESP32 supports coexistence, the examples
  deliberately avoid running both at once.  If your application needs both,
  configure WiFi/BLE coexistence in `sdkconfig` and call `begin()` after
  WiFi is set up.
- **Authentication**: The library performs "Just Works" BLE bonding automatically.
  On first use with a device that requires bonding, enable **Bluetooth pairing**
  in the Shelly app / web UI so that the initial bond can be established.
  See [BLE pairing / bonding](#ble-pairing--bonding-first-time-setup) above.

---

## License

MIT – see [LICENSE](LICENSE).
