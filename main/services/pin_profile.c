#include "pin_profile.h"

#include "sdkconfig.h"

/* Logical pin 0..7 per target. Pins claimed by the host transport, flash,
 * PSRAM, strapping or USB-JTAG are deliberately absent, so a host cannot
 * disturb the link by driving them. */
#if CONFIG_IDF_TARGET_ESP32S3

/* USB transport occupies GPIO19/20; flash and PSRAM take GPIO26-32; GPIO0, 3,
 * 45 and 46 are strapping pins. */
static const wlh_pin_descriptor_t pins[] = {
    {0u, GPIO_NUM_4, true, ADC_CHANNEL_3},
    {1u, GPIO_NUM_5, true, ADC_CHANNEL_4},
    {2u, GPIO_NUM_6, true, ADC_CHANNEL_5},
    {3u, GPIO_NUM_7, true, ADC_CHANNEL_6},
    {4u, GPIO_NUM_15, false, ADC_CHANNEL_0},
    {5u, GPIO_NUM_16, false, ADC_CHANNEL_0},
    {6u, GPIO_NUM_17, false, ADC_CHANNEL_0},
    {7u, GPIO_NUM_18, false, ADC_CHANNEL_0},
};

#elif CONFIG_IDF_TARGET_ESP32C6

/* SDIO slave occupies GPIO18-23; flash takes GPIO24-30; GPIO4, 5, 8, 9 and 15
 * are strapping pins; GPIO12/13 are USB-JTAG. */
static const wlh_pin_descriptor_t pins[] = {
    {0u, GPIO_NUM_0, true, ADC_CHANNEL_0},
    {1u, GPIO_NUM_1, true, ADC_CHANNEL_1},
    {2u, GPIO_NUM_2, true, ADC_CHANNEL_2},
    {3u, GPIO_NUM_3, true, ADC_CHANNEL_3},
    {4u, GPIO_NUM_6, true, ADC_CHANNEL_6},
    {5u, GPIO_NUM_7, false, ADC_CHANNEL_0},
    {6u, GPIO_NUM_10, false, ADC_CHANNEL_0},
    {7u, GPIO_NUM_11, false, ADC_CHANNEL_0},
};

#else
#error "No WL-hosted pin profile for this target"
#endif

const wlh_pin_descriptor_t *wlh_pin_profile_lookup(uint32_t pin_id) {
    size_t index;

    for (index = 0u; index < sizeof(pins) / sizeof(pins[0]); ++index) {
        if (pins[index].pin_id == pin_id)
            return &pins[index];
    }
    return NULL;
}

size_t wlh_pin_profile_count(void) {
    return sizeof(pins) / sizeof(pins[0]);
}
