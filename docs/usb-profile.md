# USB Profile: espressif.esp32s3.coreboard.usb-wifi

WL-hosted USB transport binding profile for the ESP32-S3 Coprocessor
firmware built from this repository. Follows
`wl-hosted-protocol/spec/transports/usb.md`.

## Enumeration

| Item | Value |
|---|---|
| VID / PID | `0x303A` / `0x8201` (Espressif VID, development PID; a product must allocate an official VID/PID) |
| Device class | 0x00 (per-interface), USB 2.0, `bcdDevice` 0x0100 |
| Interface 0 | class 0xFF (vendor-specific), subclass 0x00, protocol 0x00 |
| Bulk OUT | EP `0x01`, Host → Device, wMaxPacketSize 64 (Full Speed) |
| Bulk IN | EP `0x81`, Device → Host, wMaxPacketSize 64 (Full Speed) |
| Interrupt IN | not implemented (optional wake/doorbell endpoint) |
| Strings | manufacturer `WL-hosted`, product `WL-hosted ESP32-S3 Coprocessor`, serial = factory STA MAC hex |

## Framing

- The bulk byte stream carries raw standard WL-hosted frames starting at the
  24-byte frame header (magic `0x4C57`, protocol major 1). No IPC record or
  other encapsulation is added.
- USB packet boundaries have no frame semantics: one frame may span several
  bulk packets, one bulk transfer may contain several frames. Short packets
  and ZLP terminate transfers only, never frames.
- Reassembly reads the little-endian `payload_size` at header offset 6 and
  waits for `24 + payload_size` bytes before validating the frame.
- Aggregation (`AGGREGATED` flag) is not used in this profile version; each
  frame carries exactly one payload. Unknown record types must still be
  skipped, never fatal.
- Negotiated `max_frame_size` is 4096 bytes including the header. Checksums
  use SUM32 (default) unless CRC32C is negotiated later.

## Reset and recovery

- USB bus reset, disconnect, or re-enumeration invalidates the session: the
  firmware restarts the Coprocessor Core and both sides re-run Link Hello
  with a fresh session id. Frames from the old session are dropped.
- The host-side reference implementation is the libusb transport in
  `wl-hosted-host-macos-sim` (`--usb 303A:8201`), which reopens the device
  with a bounded wait and restarts the link on hotplug.
