#ifndef WLH_OTA_BACKEND_H
#define WLH_OTA_BACKEND_H

#include "wlh/coproc.h"

wlh_coproc_ota_ops_t wlh_ota_backend_ops(void);
int wlh_ota_backend_init(wlh_coproc_t *coproc);
void wlh_ota_backend_reset(void);

#endif
