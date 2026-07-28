#ifndef WLH_BLUETOOTH_BACKEND_H
#define WLH_BLUETOOTH_BACKEND_H

#include "wlh/coproc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP VHCI backend for the Coprocessor Core Bluetooth Controller service.
 * Lifecycle operations are queued to a dedicated `wlh-bt` task and complete
 * through the wlh_coproc_bluetooth_* ingress APIs. HCI traffic moves through
 * fixed slot rings in both directions; nothing is allocated after init.
 */

/* wlh_coproc_config_t.bluetooth ops. Valid before backend init; the ops only
 * become live after wlh_bluetooth_backend_init(). */
wlh_coproc_bluetooth_ops_t wlh_bluetooth_backend_ops(void);

/* Creates the slot rings, queues and the wlh-bt task. Call once, after
 * wlh_coproc_init() and before wlh_coproc_start(). */
int wlh_bluetooth_backend_init(wlh_coproc_t *coproc);

/* Synchronously disables and deinitializes the controller and clears all
 * HCI slots and pending lifecycle work. Call from the link control task
 * before restarting the Core; bounded by an internal timeout. */
void wlh_bluetooth_backend_reset(void);

#ifdef __cplusplus
}
#endif
#endif
