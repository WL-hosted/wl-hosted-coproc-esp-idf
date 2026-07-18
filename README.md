# WL-hosted Coprocessor ESP-IDF Adapter

ESP32-S3 Coprocessor firmware for WL-hosted. It adapts the platform-agnostic
`wl-hosted-coproc-core` to ESP-IDF: FreeRTOS OSAL, CherryUSB USB device bulk
transport, and the `esp_wifi` STA backend. Firmware profile:
`espressif.esp32s3.coreboard.usb-wifi`.

```text
Host (POSIX host-sim over USB bulk)
  ↕ raw WL-hosted frames (24-byte wire header, no extra encapsulation)
ESP32-S3 (this firmware)
  ├─ CherryUSB device, vendor interface, bulk OUT 0x01 / bulk IN 0x81
  ├─ wl-hosted-coproc-core (link/session/credit/RPC, FreeRTOS OSAL)
  └─ esp_wifi STA backend + Device Information + User Passthrough
```

Implemented services: Wi-Fi (initialize/scan/connect/disconnect, Ethernet
TX/RX), Device Information, User Passthrough (RPC SEND + optional RESULT
event). Bluetooth, OTA, extended Diagnostics, IO, ADC, KV and the
User-Passthrough raw stream channel are intentionally not implemented.

## Layout

```text
main/
├── app_main.c            # assembly: core config, tasks, lifecycle
├── coproc-core/common/osal/src/freertos_osal.c # FreeRTOS OSAL (all 30 wlh_osal ops)
├── coproc-core/common/osal/include/wlh/freertos_osal.h # FreeRTOS OSAL header
├── transport_usb.c/h     # CherryUSB bulk transport + frame reassembly
├── wifi_backend.c/h      # esp_wifi STA backend (ops + events + L2 path)
├── device_info.c/h       # Device Information provider
└── user_passthrough.c/h  # User Passthrough endpoint (echo RESULT)
coproc-core/              # submodule (protocol/ and common/ nested inside)
docs/usb-profile.md       # USB binding profile (VID/PID, endpoints, rules)
```

## Build

Requirements: ESP-IDF v5.5 (`$IDF_PATH/export.sh` sourced), `protoc` 34.1 and
`protoc-gen-nanopb` 0.4.9.1 on `PATH` (the nested Protocol submodule generates
nanopb code at build time), and network access for the IDF component manager
to fetch `cherry-embedded/cherryusb` 1.6.1 on first build.

```sh
git submodule update --init --recursive
idf.py set-target esp32s3 build
idf.py flash monitor
```

## Interop with the macOS host simulator

```sh
wl-hosted-host-macos-sim --usb 303A:8201 --scenario connect \
    --ssid <AP> --credential <passphrase>
wl-hosted-host-macos-sim --usb 303A:8201 --scenario services
```

USB bus reset or re-enumeration restarts the Core and renegotiates Hello
with a fresh session, per `protocol/spec/transports/usb.md`.

## Notes

- The Core task uses an 8 KB stack (large on-stack RPC buffers); TX/RX and
  link-control tasks use 4 KB. `CONFIG_FREERTOS_HZ=1000` gives the OSAL a
  real 1 ms monotonic clock.
- OSAL objects allocate their native handles dynamically; the Core itself
  keeps its bounded/static resource model.
- `SUBMODULE.lock` records the pinned Core commit and the transitive
  Protocol/Common commits.
