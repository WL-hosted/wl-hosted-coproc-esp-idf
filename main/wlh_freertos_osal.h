#ifndef WLH_FREERTOS_OSAL_H
#define WLH_FREERTOS_OSAL_H

#include "wlh/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FreeRTOS OSAL adapter for the WL-hosted Coprocessor Core. Objects keep
 * native handles behind the opaque OSAL storage; all waits are bounded
 * except the explicit WLH_OSAL_WAIT_FOREVER.
 */

typedef struct wlh_freertos_osal {
    uint32_t reserved;
} wlh_freertos_osal_t;

void wlh_freertos_osal_init(wlh_freertos_osal_t *osal);
wlh_osal_ops_t wlh_freertos_osal_ops(wlh_freertos_osal_t *osal);

#ifdef __cplusplus
}
#endif
#endif
