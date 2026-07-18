#include <string.h>

#include "esp_mac.h"
#include "wlh/coproc.h"

#define WLH_BOARD_PROFILE "espressif.esp32s3.coreboard.usb-wifi"

/* wlh_coproc_get_device_info_fn for wlh_coproc_config_t.device_info. */
static int get_device_info(void *context, wlh_coproc_device_info_t *info) {
    (void)context;
    if (info == NULL)
        return -1;
    memset(info, 0, sizeof(*info));
    memcpy(info->vendor, "espressif", sizeof("espressif"));
    memcpy(info->mcu_model, "ESP32-S3", sizeof("ESP32-S3"));
    memcpy(info->board_profile, WLH_BOARD_PROFILE, sizeof(WLH_BOARD_PROFILE));
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
