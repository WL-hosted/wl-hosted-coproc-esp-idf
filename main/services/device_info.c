#include <string.h>

#include "esp_mac.h"
#include "firmware_config.h"
#include "wlh/coproc.h"

/* wlh_coproc_get_device_info_fn for wlh_coproc_config_t.device_info. */
static int get_device_info(void *context, wlh_coproc_device_info_t *info) {
    (void)context;
    if (info == NULL)
        return -1;
    memset(info, 0, sizeof(*info));
    memcpy(info->vendor, "espressif", sizeof("espressif"));
    memcpy(info->mcu_model, WLH_MCU_NAME, sizeof(WLH_MCU_NAME));
    strncpy(
        info->board_profile, WLH_BOARD_PROFILE, sizeof(info->board_profile) - 1u
    );
    if (esp_read_mac(info->uid, ESP_MAC_WIFI_STA) != 0)
        info->uid_size = 0u;
    else
        info->uid_size = 6u;
    return 0;
}

wlh_coproc_device_info_ops_t wlh_device_info_ops(void) {
    wlh_coproc_device_info_ops_t ops = {NULL, get_device_info};
    return ops;
}
