#ifndef WLH_USER_PASSTHROUGH_H
#define WLH_USER_PASSTHROUGH_H

#include "wlh/coproc.h"

#ifdef __cplusplus
extern "C" {
#endif

wlh_coproc_user_passthrough_ops_t wlh_user_passthrough_ops(
    wlh_coproc_t *coproc
);

#ifdef __cplusplus
}
#endif
#endif
