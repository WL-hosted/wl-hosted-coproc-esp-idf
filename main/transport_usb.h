#ifndef WLH_TRANSPORT_USB_H
#define WLH_TRANSPORT_USB_H

#include <stddef.h>
#include <stdint.h>

#include "wlh/coproc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB device transport (CherryUSB, DWC2, bulk) for the WL-hosted
 * Coprocessor Core. Implements the USB binding profile
 * espressif.esp32s3.coreboard.usb-wifi: one vendor-specific interface with
 * a bulk OUT endpoint (Host to Device) and a bulk IN endpoint (Device to
 * Host). The bulk byte stream carries raw WL-hosted frames; USB packet
 * boundaries have no frame semantics, so the receiver reassembles frames
 * with the 24-byte wire header.
 */

/* Called from the transport control context (task, not ISR) when the USB
 * bus is reset or re-enumerated. The link must renegotiate Hello with a
 * fresh session afterwards. */
typedef void (*wlh_usb_bus_reset_fn)(void *context);

typedef struct wlh_usb_transport_config {
    wlh_coproc_t *coproc;
    size_t max_frame_size;
    wlh_usb_bus_reset_fn on_bus_reset;
    void *bus_reset_context;
} wlh_usb_transport_config_t;

/* Starts the USB device stack and the transport tasks. */
int wlh_usb_transport_start(const wlh_usb_transport_config_t *config);

/* wlh_coproc_submit_tx_fn implementation. */
int wlh_usb_transport_submit_tx(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
);

#ifdef __cplusplus
}
#endif
#endif
