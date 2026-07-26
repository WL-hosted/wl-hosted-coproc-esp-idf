#ifndef WLH_PIN_PROFILE_H
#define WLH_PIN_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal/adc_types.h"
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The wire pin_id is a logical index into the board profile, not a vendor GPIO
 * number. Keeping the mapping here is what lets each target expose only the
 * pins its transport, flash and strapping pins leave free. */
typedef struct wlh_pin_descriptor {
    uint32_t pin_id;
    gpio_num_t gpio;
    /* ADC1 only: ADC2 is unavailable while Wi-Fi is running. */
    bool adc_supported;
    adc_channel_t adc_channel;
} wlh_pin_descriptor_t;

/* Returns NULL when the profile does not publish pin_id. */
const wlh_pin_descriptor_t *wlh_pin_profile_lookup(uint32_t pin_id);

size_t wlh_pin_profile_count(void);

#ifdef __cplusplus
}
#endif
#endif
