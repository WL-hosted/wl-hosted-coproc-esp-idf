#include "io_service.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "pin_profile.h"

static const char *TAG = "wlh-io";

/* READ must report the configuration in effect rather than the last request,
 * and WRITE must refuse a pin that is not an output, so the effective mode of
 * each logical pin is tracked here. Both are reached only from the Core task,
 * so no lock is needed. */
typedef struct pin_state {
    bool configured;
    wlh_coproc_io_mode_t mode;
    wlh_coproc_io_pull_t pull;
} pin_state_t;

static pin_state_t pin_states[8];

_Static_assert(
    sizeof(pin_states) / sizeof(pin_states[0]) >= 8u,
    "pin state table smaller than the published profile"
);

static pin_state_t *state_for(uint32_t pin_id) {
    if (pin_id >= sizeof(pin_states) / sizeof(pin_states[0]))
        return NULL;
    return &pin_states[pin_id];
}

static int configure(void *context, const wlh_coproc_io_config_t *config) {
    const wlh_pin_descriptor_t *pin;
    pin_state_t *state;
    gpio_config_t io = {0};
    esp_err_t error;

    (void)context;
    if (config == NULL)
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;
    pin = wlh_pin_profile_lookup(config->pin_id);
    state = state_for(config->pin_id);
    if (pin == NULL || state == NULL)
        return WLH_COPROC_SERVICE_NOT_FOUND;

    io.pin_bit_mask = 1ULL << (unsigned)pin->gpio;
    io.intr_type = GPIO_INTR_DISABLE;
    io.pull_up_en = config->pull == WLH_COPROC_IO_PULL_UP ? GPIO_PULLUP_ENABLE
                                                          : GPIO_PULLUP_DISABLE;
    io.pull_down_en = config->pull == WLH_COPROC_IO_PULL_DOWN
                          ? GPIO_PULLDOWN_ENABLE
                          : GPIO_PULLDOWN_DISABLE;
    switch (config->mode) {
    case WLH_COPROC_IO_MODE_INPUT:
        io.mode = GPIO_MODE_INPUT;
        break;
    case WLH_COPROC_IO_MODE_OUTPUT:
        io.mode = GPIO_MODE_OUTPUT;
        break;
    default:
        io.mode = GPIO_MODE_OUTPUT_OD;
        break;
    }

    /* Latch the requested level before the direction change so an output does
     * not glitch through the previous latch value. */
    if (config->mode != WLH_COPROC_IO_MODE_INPUT) {
        error = gpio_set_level(pin->gpio, config->initial_level ? 1u : 0u);
        if (error != ESP_OK) {
            ESP_LOGW(
                TAG,
                "pin %u pre-latch failed: %s",
                (unsigned)config->pin_id,
                esp_err_to_name(error)
            );
            return WLH_COPROC_SERVICE_INTERNAL;
        }
    }

    error = gpio_config(&io);
    if (error != ESP_OK) {
        ESP_LOGW(
            TAG,
            "pin %u (gpio %d) configure failed: %s",
            (unsigned)config->pin_id,
            (int)pin->gpio,
            esp_err_to_name(error)
        );
        state->configured = false;
        return WLH_COPROC_SERVICE_INTERNAL;
    }
    if (config->mode != WLH_COPROC_IO_MODE_INPUT)
        (void)gpio_set_level(pin->gpio, config->initial_level ? 1u : 0u);

    state->configured = true;
    state->mode = config->mode;
    state->pull = config->pull;
    return WLH_COPROC_SERVICE_OK;
}

static int read_pin(
    void *context, uint32_t pin_id, wlh_coproc_io_state_t *out
) {
    const wlh_pin_descriptor_t *pin = wlh_pin_profile_lookup(pin_id);
    const pin_state_t *state = state_for(pin_id);

    (void)context;
    if (out == NULL)
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;
    if (pin == NULL || state == NULL)
        return WLH_COPROC_SERVICE_NOT_FOUND;
    /* Nothing has been configured, so there is no effective mode to report. */
    if (!state->configured)
        return WLH_COPROC_SERVICE_INVALID_STATE;

    memset(out, 0, sizeof(*out));
    out->level = gpio_get_level(pin->gpio) != 0;
    out->mode = state->mode;
    out->pull = state->pull;
    return WLH_COPROC_SERVICE_OK;
}

static int write_pin(void *context, uint32_t pin_id, bool level) {
    const wlh_pin_descriptor_t *pin = wlh_pin_profile_lookup(pin_id);
    const pin_state_t *state = state_for(pin_id);
    esp_err_t error;

    (void)context;
    if (pin == NULL || state == NULL)
        return WLH_COPROC_SERVICE_NOT_FOUND;
    /* Writing an unconfigured or input pin has no defined effect. For
     * OPEN_DRAIN, true releases the line and false pulls it low. */
    if (!state->configured || state->mode == WLH_COPROC_IO_MODE_INPUT)
        return WLH_COPROC_SERVICE_INVALID_STATE;

    error = gpio_set_level(pin->gpio, level ? 1u : 0u);
    if (error != ESP_OK) {
        ESP_LOGW(
            TAG,
            "pin %u write failed: %s",
            (unsigned)pin_id,
            esp_err_to_name(error)
        );
        return WLH_COPROC_SERVICE_INTERNAL;
    }
    return WLH_COPROC_SERVICE_OK;
}

wlh_coproc_io_ops_t wlh_io_ops(void) {
    wlh_coproc_io_ops_t ops = {NULL, configure, read_pin, write_pin};
    return ops;
}
