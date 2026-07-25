#ifndef WLH_FIRMWARE_CONFIG_H
#define WLH_FIRMWARE_CONFIG_H

#include "sdkconfig.h"

#define WLH_MCU_NAME CONFIG_IDF_TARGET

#if CONFIG_WLH_TRANSPORT_USB
#define WLH_TRANSPORT_NAME "usb"
#define WLH_PROFILE_BOARD_SEGMENT "coreboard."
#elif CONFIG_WLH_TRANSPORT_SDIO
#define WLH_TRANSPORT_NAME "sdio"
#define WLH_PROFILE_BOARD_SEGMENT ""
#else
#error "No WL-hosted transport selected"
#endif

#define WLH_BOARD_PROFILE                                                      \
    "espressif." CONFIG_IDF_TARGET                                             \
    "." WLH_PROFILE_BOARD_SEGMENT WLH_TRANSPORT_NAME "-wifi"

#endif
