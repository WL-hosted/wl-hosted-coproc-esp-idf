#ifndef WLH_TRANSPORT_H
#define WLH_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "wlh/coproc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called in task context after the active transport has reset. */
typedef void (*wlh_transport_reset_fn)(void *context);

typedef struct wlh_transport_config {
    wlh_coproc_t *coproc;
    size_t max_frame_size;
    wlh_transport_reset_fn on_reset;
    void *reset_context;
} wlh_transport_config_t;

int wlh_transport_start(const wlh_transport_config_t *config);

/* wlh_coproc_submit_tx_fn implementation. */
int wlh_transport_submit_tx(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
);

size_t wlh_transport_max_frame_size(void);

#ifdef __cplusplus
}
#endif
#endif
