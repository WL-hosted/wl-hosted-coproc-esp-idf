#include "adc_service.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "pin_profile.h"

#define WLH_ADC_ATTENUATION ADC_ATTEN_DB_12

static const char *TAG = "wlh-adc";

/* Unit and per-channel calibration handles are created on first use: a board
 * that never reads an ADC pin should not pay for the peripheral. Reached only
 * from the Core task. */
static adc_oneshot_unit_handle_t unit;
static adc_cali_handle_t calibrations[ADC_CHANNEL_7 + 1];

static bool ensure_unit(void) {
    adc_oneshot_unit_init_cfg_t config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = 0,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t error;

    if (unit != NULL)
        return true;
    error = adc_oneshot_new_unit(&config, &unit);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 unit init failed: %s", esp_err_to_name(error));
        unit = NULL;
        return false;
    }
    return true;
}

/* Calibration is what turns a raw code into a voltage; without it there is no
 * millivolt answer to report. */
static bool ensure_calibration(adc_channel_t channel) {
    esp_err_t error;

    if (channel > ADC_CHANNEL_7)
        return false;
    if (calibrations[channel] != NULL)
        return true;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t config = {
            .unit_id = ADC_UNIT_1,
            .chan = channel,
            .atten = WLH_ADC_ATTENUATION,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        error = adc_cali_create_scheme_curve_fitting(
            &config, &calibrations[channel]
        );
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t config = {
            .unit_id = ADC_UNIT_1,
            .atten = WLH_ADC_ATTENUATION,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        error = adc_cali_create_scheme_line_fitting(
            &config, &calibrations[channel]
        );
    }
#else
#error "No ADC calibration scheme available for this target"
#endif
    if (error != ESP_OK) {
        ESP_LOGW(
            TAG,
            "channel %d calibration failed: %s",
            (int)channel,
            esp_err_to_name(error)
        );
        calibrations[channel] = NULL;
        return false;
    }
    return true;
}

static int read_pin(void *context, uint32_t pin_id, uint32_t *millivolts) {
    const wlh_pin_descriptor_t *pin = wlh_pin_profile_lookup(pin_id);
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = WLH_ADC_ATTENUATION,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    int raw = 0;
    int voltage = 0;
    esp_err_t error;

    (void)context;
    if (millivolts == NULL)
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;
    if (pin == NULL)
        return WLH_COPROC_SERVICE_NOT_FOUND;
    if (!pin->adc_supported)
        return WLH_COPROC_SERVICE_NOT_SUPPORTED;
    if (!ensure_unit())
        return WLH_COPROC_SERVICE_INTERNAL;

    error = adc_oneshot_config_channel(unit, pin->adc_channel, &channel_config);
    if (error != ESP_OK) {
        ESP_LOGW(
            TAG,
            "pin %u channel config failed: %s",
            (unsigned)pin_id,
            esp_err_to_name(error)
        );
        return WLH_COPROC_SERVICE_INTERNAL;
    }
    error = adc_oneshot_read(unit, pin->adc_channel, &raw);
    if (error != ESP_OK) {
        ESP_LOGW(
            TAG,
            "pin %u read failed: %s",
            (unsigned)pin_id,
            esp_err_to_name(error)
        );
        return WLH_COPROC_SERVICE_INTERNAL;
    }
    if (!ensure_calibration(pin->adc_channel))
        return WLH_COPROC_SERVICE_INTERNAL;
    error =
        adc_cali_raw_to_voltage(calibrations[pin->adc_channel], raw, &voltage);
    if (error != ESP_OK || voltage < 0) {
        ESP_LOGW(
            TAG,
            "pin %u conversion failed: %s",
            (unsigned)pin_id,
            esp_err_to_name(error)
        );
        return WLH_COPROC_SERVICE_INTERNAL;
    }
    *millivolts = (uint32_t)voltage;
    return WLH_COPROC_SERVICE_OK;
}

wlh_coproc_adc_ops_t wlh_adc_ops(void) {
    wlh_coproc_adc_ops_t ops = {NULL, read_pin};
    return ops;
}
