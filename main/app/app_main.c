#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "adc_service.h"
#include "device_info.h"
#include "esp_app_desc.h"
#include "firmware_config.h"
#include "io_service.h"
#include "kv_service.h"
#include "ota_backend.h"
#include "transport.h"
#include "user_passthrough.h"
#include "wifi_backend.h"
#include "wlh/coproc.h"
#include "wlh/freertos_osal.h"

#if CONFIG_WLH_ENABLE_BLUETOOTH_CONTROLLER
#include "bluetooth_backend.h"
#endif

#pragma message("WLH PROFILE: " WLH_BOARD_PROFILE)

#define LINK_EVENT_TRANSPORT_RESET (1u << 0)

static const char *TAG = "wlh-coproc";

static wlh_coproc_t coproc;
static wlh_freertos_osal_t freertos_osal;
static EventGroupHandle_t link_events;

static uint8_t *buffer_alloc(void *context, size_t size) {
    (void)context;
    return malloc(size);
}
static void buffer_free(void *context, uint8_t *buffer) {
    (void)context;
    free(buffer);
}

/* Called in task context after the active transport has reset. */
static void on_transport_reset(void *context) {
    (void)context;
    xEventGroupSetBits(link_events, LINK_EVENT_TRANSPORT_RESET);
}

/* Restarts the Core after a transport reset so the link renegotiates Hello
 * with a fresh session. */
static void link_control_task(void *argument) {
    (void)argument;
    for (;;) {
        xEventGroupWaitBits(
            link_events,
            LINK_EVENT_TRANSPORT_RESET,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );
        /* Let the re-enumeration settle before re-accepting frames. */
        vTaskDelay(pdMS_TO_TICKS(100u));
        ESP_LOGW(TAG, "transport reset: restarting link core");
#if CONFIG_WLH_ENABLE_BLUETOOTH_CONTROLLER
        wlh_bluetooth_backend_reset();
#endif
        wlh_ota_backend_reset();
        if (wlh_coproc_stop(&coproc) != WLH_COPROC_OK)
            ESP_LOGW(TAG, "core stop failed during restart");
        if (wlh_coproc_start(&coproc) != WLH_COPROC_OK)
            ESP_LOGE(TAG, "core restart failed");
    }
}

void app_main(void) {
    wlh_coproc_config_t config;
    wlh_transport_config_t transport_config;

    ESP_LOGI(
        TAG,
        "wl-hosted %s coprocessor (transport %s, profile %s)",
        WLH_MCU_NAME,
        WLH_TRANSPORT_NAME,
        WLH_BOARD_PROFILE
    );

    /* The KV service persists host data here, so a partition left over from a
     * different NVS layout is erased rather than ignored. */
    {
        esp_err_t nvs_status = nvs_flash_init();
        if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "erasing incompatible NVS partition");
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_status = nvs_flash_init();
        }
        if (nvs_status != ESP_OK)
            ESP_LOGE(
                TAG,
                "NVS unavailable, KV service will fail: %s",
                esp_err_to_name(nvs_status)
            );
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    link_events = xEventGroupCreate();
    configASSERT(link_events != NULL);

    wlh_freertos_osal_init(&freertos_osal);

    memset(&config, 0, sizeof(config));
    config.port.context = NULL;
    config.port.submit_tx = wlh_transport_submit_tx;
    config.port.ethernet_rx = wlh_wifi_backend_ethernet_tx;
    config.port.ethernet_ap_rx = wlh_wifi_backend_ethernet_ap_tx;
    config.buffers = (wlh_coproc_buffer_ops_t){NULL, buffer_alloc, buffer_free};
    config.osal = wlh_freertos_osal_ops(&freertos_osal);
    config.wifi = (wlh_coproc_wifi_ops_t){NULL,
                                          wlh_wifi_backend_initialize,
                                          wlh_wifi_backend_scan,
                                          wlh_wifi_backend_connect,
                                          wlh_wifi_backend_disconnect,
                                          wlh_wifi_backend_start_ap,
                                          wlh_wifi_backend_stop_ap};
    config.device_info = wlh_device_info_ops();
    config.user_passthrough = wlh_user_passthrough_ops(&coproc);
    config.io = wlh_io_ops();
    config.adc = wlh_adc_ops();
    config.kv = wlh_kv_ops();
    config.ota = wlh_ota_backend_ops();
    strncpy(
        config.implementation_version,
        esp_app_get_description()->version,
        sizeof(config.implementation_version) - 1u
    );
#if CONFIG_WLH_ENABLE_BLUETOOTH_CONTROLLER
    config.bluetooth = wlh_bluetooth_backend_ops();
#endif
    if (wlh_ota_backend_init(&coproc) != 0) {
        ESP_LOGE(TAG, "OTA backend init failed");
        abort();
    }
    config.max_frame_size = wlh_transport_max_frame_size();
    config.heartbeat_interval_ms = 1000u;
    config.initial_credit = 64u;
    config.core_queue_depth = 16u;
    config.stop_timeout_ms = 3000u;
    config.core_task = (wlh_osal_task_attributes_t){"wlh-core", 8192u, 7};

    if (wlh_coproc_init(&coproc, &config) != WLH_COPROC_OK) {
        ESP_LOGE(TAG, "coproc init failed");
        abort();
    }
    if (wlh_wifi_backend_init(&coproc) != 0) {
        ESP_LOGE(TAG, "wifi backend init failed");
        abort();
    }
#if CONFIG_WLH_ENABLE_BLUETOOTH_CONTROLLER
    if (wlh_bluetooth_backend_init(&coproc) != 0) {
        ESP_LOGE(TAG, "bluetooth backend init failed");
        abort();
    }
#endif

    memset(&transport_config, 0, sizeof(transport_config));
    transport_config.coproc = &coproc;
    transport_config.max_frame_size = config.max_frame_size;
    transport_config.on_reset = on_transport_reset;
    transport_config.reset_context = NULL;

    if (wlh_coproc_start(&coproc) != WLH_COPROC_OK) {
        ESP_LOGE(TAG, "coproc start failed");
        abort();
    }
    if (wlh_transport_start(&transport_config) != 0) {
        ESP_LOGE(TAG, "%s transport start failed", WLH_TRANSPORT_NAME);
        abort();
    }
    if (xTaskCreate(link_control_task, "wlh-link-ctrl", 4096u, NULL, 7, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "link control task creation failed");
        abort();
    }
    ESP_LOGI(TAG, "coprocessor ready, waiting for host");
}
