#ifndef WLH_WIFI_BACKEND_H
#define WLH_WIFI_BACKEND_H

#include "wlh/coproc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * esp_wifi backend for the Coprocessor Core. The interface mode (STA/AP/APSTA)
 * is selected from the host's WifiInitializeRequest.interface_flags. The wifi
 * ops are nonblocking submissions; scan/connect/initialize results and SoftAP
 * client join/leave events reach the Core through the wlh_coproc_wifi_*
 * ingress APIs from the Wi-Fi event handler.
 */

/* Registers the Wi-Fi event handler and receive callbacks. The
 * default event loop must already exist. */
int wlh_wifi_backend_init(wlh_coproc_t *coproc);

/* wlh_wifi_*_fn implementations for wlh_coproc_config_t.wifi. */
int wlh_wifi_backend_initialize(
    void *context, uint32_t operation_id, uint32_t interface_flags
);
int wlh_wifi_backend_scan(void *context, uint32_t scan_id);
int wlh_wifi_backend_connect(
    void *context, const wlh_coproc_wifi_connect_t *request
);
int wlh_wifi_backend_disconnect(void *context);
int wlh_wifi_backend_start_ap(
    void *context, const wlh_coproc_wifi_ap_t *request
);
int wlh_wifi_backend_stop_ap(void *context);

/* wlh_coproc_ethernet_rx_fn: forwards a Host frame to the AP. Runs on the
 * Core task and returns immediately; frames are dropped when not
 * connected. */
void wlh_wifi_backend_ethernet_tx(
    void *context, const uint8_t *frame, size_t size
);
void wlh_wifi_backend_ethernet_ap_tx(
    void *context, const uint8_t *frame, size_t size
);

#ifdef __cplusplus
}
#endif
#endif
