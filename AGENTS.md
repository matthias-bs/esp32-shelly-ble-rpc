# AGENTS.md

Guidance for AI coding agents working in this repository.

## Scope

- Repository: esp32-shelly-ble-rpc
- Primary target: ESP32 Arduino library for Shelly Gen2+ JSON-RPC over BLE
- Canonical project documentation: [README.md](README.md)

## First Read

Before editing code, read these files first:

- [README.md](README.md)
- [src/ShellyBleRpc.h](src/ShellyBleRpc.h)
- [src/ShellyBleRpc.cpp](src/ShellyBleRpc.cpp)
- [examples/ShellyBleFixed/ShellyBleFixed.ino](examples/ShellyBleFixed/ShellyBleFixed.ino)
- [examples/ShellyBleScanConnect/ShellyBleScanConnect.ino](examples/ShellyBleScanConnect/ShellyBleScanConnect.ino)
- [examples/ShellyBleWiFiConfig/ShellyBleWiFiConfig.ino](examples/ShellyBleWiFiConfig/ShellyBleWiFiConfig.ino)

For CI behavior and supported build variants:

- [.github/workflows/CI.yml](.github/workflows/CI.yml)
- [.github/workflows/arduino-lint.yml](.github/workflows/arduino-lint.yml)

## Build And Validation

Use arduino-cli locally. CI compiles all examples for ESP32 variants.

1. Install required dependency:

```bash
arduino-cli lib install NimBLE-Arduino@2.5.0
```

2. Compile one example (fast local check):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:DebugLevel=none examples/ShellyBleFixed
```

3. Compile all examples (CI-like loop):

```bash
for example in $(find examples -name '*.ino' | sort); do
  arduino-cli compile --fqbn esp32:esp32:esp32:DebugLevel=none "$example" --warnings=all || exit 1
done
```

4. Run Arduino lint (same policy as CI workflow):

```bash
arduino-lint --library-manager update --compliance strict
```

## Architecture Map

- Public API and constants: [src/ShellyBleRpc.h](src/ShellyBleRpc.h)
- BLE transport and RPC implementation: [src/ShellyBleRpc.cpp](src/ShellyBleRpc.cpp)
- Metadata and dependency declaration: [library.properties](library.properties)
- Usage patterns:
  - Fixed-address connect: [examples/ShellyBleFixed/ShellyBleFixed.ino](examples/ShellyBleFixed/ShellyBleFixed.ino)
  - Scan-and-connect flow: [examples/ShellyBleScanConnect/ShellyBleScanConnect.ino](examples/ShellyBleScanConnect/ShellyBleScanConnect.ino)
  - WiFi config portal then BLE runtime: [examples/ShellyBleWiFiConfig/ShellyBleWiFiConfig.ino](examples/ShellyBleWiFiConfig/ShellyBleWiFiConfig.ino)

## Project-Specific Rules

- Preserve public API compatibility unless a breaking change is explicitly requested.
- Keep behavior aligned with documented constraints in [README.md](README.md):
  - Only one ShellyBleRpc instance should be active at a time.
  - Do not call call() concurrently from multiple tasks.
  - Keep WiFi AP config mode and BLE runtime mode separated unless explicitly implementing coexistence.
- Keep UUIDs and wire protocol framing consistent with [README.md](README.md) and [src/ShellyBleRpc.h](src/ShellyBleRpc.h).
- Maintain Arduino-compatible C++ style already used in src and examples.

## Change Checklist

For changes in src:

1. Rebuild at least one example that exercises the changed path.
2. If RPC framing, scan, or connection flow changed, compile all examples.
3. Update [README.md](README.md) when API or behavior changes.
4. Keep examples runnable and configuration constants near the top of each sketch.

## Common Failure Modes

- Connection/authentication failures may require Shelly Bluetooth pairing mode for first bond. See pairing guidance in [README.md](README.md).
- Begin/connect ordering matters: initialize BLE stack before scan/connect.
- If changing scan logic, preserve both match paths currently used by the library:
  - advertised RPC service UUID
  - advertised name starts with Shelly

## Suggested Next Customizations

If this repository grows, add focused customizations:

- .github/instructions/examples.instructions.md
  - Purpose: rules specific to editing sketches under examples/
- .github/instructions/src.instructions.md
  - Purpose: API-compatibility and transport invariants for src/
- .github/prompts/compile-all.prompt.md
  - Purpose: reusable prompt to compile all examples with standard fqbn/warnings
