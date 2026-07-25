# ESP32-C6 SDIO Binding Profile

This document defines the Coprocessor side of profile
`espressif.esp32c6.sdio-wifi`.

## Transport contract

| Property | Value |
|---|---|
| Coprocessor | ESP32-C6 |
| SDIO function | Function 1 |
| Bus width | 4-bit |
| Block size | 512 bytes |
| Sending mode | Packet |
| DMA alignment | 4 bytes |
| Maximum transaction | 4092 bytes |
| Maximum WL-hosted frame | 4092 bytes, including the 24-byte header |

Each SDIO transaction contains exactly one complete, unmodified WL-hosted
frame. There is no ESP-Hosted private header and no transport checksum. The
standard WL-hosted header and payload checksums remain authoritative. A
transaction is rejected when its length is less than the wire header, exceeds
4092 bytes, or differs from the length encoded by the frame.

The firmware uses a bounded software TX queue, one hardware packet in flight,
and pre-registered DMA RX buffers. The single in-flight rule keeps each
packet-length delta aligned with exactly one wire frame. TX ownership returns
to Coprocessor Core only after the host consumes the SDIO packet.

## ESP32-C6 pins

The ESP32-C6 SDIO Slave peripheral uses fixed pins:

| Signal | GPIO |
|---|---:|
| CLK | 19 |
| CMD | 18 |
| DAT0 | 20 |
| DAT1 | 21 |
| DAT2 | 22 |
| DAT3 | 23 |
| Reset | EN/RST |

CMD and DAT0-DAT3 require external pull-ups; 51 kOhm is the reference value.
Internal pull-ups are exposed only for short bench tests and are disabled by
default. Keep traces short and length-matched, share ground, and start at the
default bus speed when validating a new board.

## Reset and recovery

The preferred reset is the host driving the ESP32-C6 EN/RST input. A host that
keeps the MCU running may raise Host-to-Slave interrupt bit 0 to request a link
reset. The firmware then:

1. stops and resets the SDIO function;
2. fails queued and in-flight TX frames;
3. restarts the SDIO function;
4. restarts Coprocessor Core; and
5. waits for Link Hello with a new session.

The old session must not be reused. Slave-to-Host packet availability uses the
ESP-IDF `SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET` mechanism.

## Kconfig

Select `WL-hosted Coprocessor Configuration -> Host transport -> SDIO slave`.
The ESP32-C6 target defaults to SDIO. The following settings are available:

- default or high-speed capability;
- four send/sample timing combinations;
- bounded TX queue depth;
- number of pre-registered RX DMA buffers; and
- debug-only internal pull-ups.

The repository currently provides only the Coprocessor Adapter. A compatible
Host SDIO Master must implement this profile before RPC and Wi-Fi end-to-end
tests can run.
