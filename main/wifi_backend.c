#include "wifi_backend.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"

#define WLH_WIFI_SCAN_MAX_RESULTS 24u

#define WIFI_SECURITY_OPEN 1u
#define WIFI_SECURITY_WEP 2u
#define WIFI_SECURITY_WPA_PSK 3u
#define WIFI_SECURITY_WPA2_PSK 4u
#define WIFI_SECURITY_WPA_WPA2_PSK 5u
#define WIFI_SECURITY_WPA3_SAE 6u
#define WIFI_SECURITY_WPA2_WPA3_PSK 7u

#define DISCONNECT_REASON_LOCAL_REQUEST 1u
#define DISCONNECT_REASON_AP_NOT_FOUND 2u
#define DISCONNECT_REASON_AUTH_FAILED 3u
#define DISCONNECT_REASON_ASSOC_FAILED 4u
#define DISCONNECT_REASON_HANDSHAKE_TIMEOUT 5u
#define DISCONNECT_REASON_BEACON_LOST 6u
#define DISCONNECT_REASON_PEER_DEAUTH 7u
#define DISCONNECT_REASON_LINK_LOST 8u

static const char *TAG = "wlh-wifi";

typedef struct wifi_backend {
    wlh_coproc_t *coproc;
    uint32_t initialize_operation_id;
    uint32_t scan_id;
    bool driver_started;
    bool connected;
    bool disconnect_locally;
} wifi_backend_t;

static wifi_backend_t backend;

static uint32_t map_security(wifi_auth_mode_t mode) {
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return WIFI_SECURITY_OPEN;
    case WIFI_AUTH_WEP:
        return WIFI_SECURITY_WEP;
    case WIFI_AUTH_WPA_PSK:
        return WIFI_SECURITY_WPA_PSK;
    case WIFI_AUTH_WPA2_PSK:
        return WIFI_SECURITY_WPA2_PSK;
    case WIFI_AUTH_WPA_WPA2_PSK:
        return WIFI_SECURITY_WPA_WPA2_PSK;
    case WIFI_AUTH_WPA3_PSK:
        return WIFI_SECURITY_WPA3_SAE;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return WIFI_SECURITY_WPA2_WPA3_PSK;
    default:
        return WIFI_SECURITY_WPA2_PSK;
    }
}

static uint32_t map_disconnect_reason(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return DISCONNECT_REASON_AP_NOT_FOUND;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return DISCONNECT_REASON_AUTH_FAILED;
    case WIFI_REASON_ASSOC_FAIL:
        return DISCONNECT_REASON_ASSOC_FAILED;
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return DISCONNECT_REASON_HANDSHAKE_TIMEOUT;
    case WIFI_REASON_BEACON_TIMEOUT:
        return DISCONNECT_REASON_BEACON_LOST;
    case WIFI_REASON_STA_LEAVING:
        return DISCONNECT_REASON_PEER_DEAUTH;
    default:
        return DISCONNECT_REASON_LINK_LOST;
    }
}

static esp_err_t sta_rx_callback(void *buffer, uint16_t length, void *eb) {
    (void)eb;
    /* Runs on the Wi-Fi task. The Core copies the frame into its bounded
     * queue; the driver buffer is released on return. */
    if (backend.coproc != NULL) {
        (void)wlh_coproc_ethernet_sta_send(
            backend.coproc, buffer, (size_t)length
        );
    }
    return ESP_OK;
}

static void report_scan_done(void) {
    uint16_t ap_count = 0u;
    wifi_ap_record_t *records;
    uint16_t index;

    (void)esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > WLH_WIFI_SCAN_MAX_RESULTS)
        ap_count = WLH_WIFI_SCAN_MAX_RESULTS;
    records = calloc(ap_count == 0u ? 1u : ap_count, sizeof(*records));
    if (records == NULL) {
        (void)wlh_coproc_wifi_scan_completed(
            backend.coproc, backend.scan_id, 0u, false
        );
        return;
    }
    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK)
        ap_count = 0u;

    for (index = 0; index < ap_count; ++index) {
        wlh_coproc_bss_t bss;
        size_t ssid_size = strnlen((const char *)records[index].ssid, 32u);
        memset(&bss, 0, sizeof(bss));
        bss.ssid = records[index].ssid;
        bss.ssid_size = ssid_size;
        memcpy(bss.bssid, records[index].bssid, sizeof(bss.bssid));
        bss.security = map_security(records[index].authmode);
        bss.channel = records[index].primary;
        bss.rssi_dbm = records[index].rssi;
        (void)wlh_coproc_wifi_scan_result(
            backend.coproc, backend.scan_id, &bss
        );
    }
    (void)wlh_coproc_wifi_scan_completed(
        backend.coproc, backend.scan_id, ap_count, false
    );
    free(records);
}

static void wifi_event_handler(
    void *argument, esp_event_base_t base, int32_t id, void *data
) {
    (void)argument;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (backend.initialize_operation_id != 0u) {
            uint32_t operation_id = backend.initialize_operation_id;
            backend.initialize_operation_id = 0u;
            (void)wlh_coproc_wifi_initialized(backend.coproc, operation_id, 0);
        }
        break;

    case WIFI_EVENT_SCAN_DONE:
        report_scan_done();
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *event = data;
        wifi_ap_record_t ap_info;
        wlh_coproc_bss_t bss;
        memset(&bss, 0, sizeof(bss));
        backend.connected = true;
        bss.ssid = event->ssid;
        bss.ssid_size = event->ssid_len;
        memcpy(bss.bssid, event->bssid, sizeof(bss.bssid));
        bss.channel = event->channel;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            bss.rssi_dbm = ap_info.rssi;
            bss.security = map_security(ap_info.authmode);
        }
        (void)wlh_coproc_wifi_connected(backend.coproc, &bss);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *event = data;
        bool locally_initiated = backend.disconnect_locally;
        backend.disconnect_locally = false;
        backend.connected = false;
        ESP_LOGI(TAG, "disconnected (reason=%u)", event->reason);
        (void)wlh_coproc_wifi_disconnected(
            backend.coproc,
            locally_initiated ? DISCONNECT_REASON_LOCAL_REQUEST
                              : map_disconnect_reason(event->reason),
            locally_initiated
        );
        break;
    }

    default:
        break;
    }
}

int wlh_wifi_backend_init(wlh_coproc_t *coproc) {
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (coproc == NULL)
        return -1;
    memset(&backend, 0, sizeof(backend));
    backend.coproc = coproc;
    if (esp_wifi_init(&config) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_event_handler_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL
        ) != ESP_OK) {
        ESP_LOGE(TAG, "wifi backend init failed");
        return -1;
    }
    return 0;
}

int wlh_wifi_backend_initialize(void *context, uint32_t operation_id) {
    (void)context;
    if (backend.driver_started) {
        /* Already up: report completion inline. */
        return wlh_coproc_wifi_initialized(backend.coproc, operation_id, 0) ==
                       WLH_COPROC_OK
                   ? 0
                   : -1;
    }
    if (esp_wifi_start() != ESP_OK)
        return -1;
    backend.initialize_operation_id = operation_id;
    backend.driver_started = true;
    (void)esp_wifi_internal_reg_rxcb(WIFI_IF_STA, sta_rx_callback);
    return 0;
}

int wlh_wifi_backend_scan(void *context, uint32_t scan_id) {
    (void)context;
    if (!backend.driver_started)
        return -1;
    if (esp_wifi_scan_start(NULL, false) != ESP_OK)
        return -1;
    backend.scan_id = scan_id;
    return 0;
}

int wlh_wifi_backend_connect(
    void *context, const wlh_coproc_wifi_connect_t *request
) {
    wifi_config_t config;
    (void)context;
    if (!backend.driver_started || request == NULL ||
        request->ssid_size == 0u || request->ssid_size > 32u ||
        request->credential_size > 63u) {
        return -1;
    }
    memset(&config, 0, sizeof(config));
    memcpy(config.sta.ssid, request->ssid, request->ssid_size);
    memcpy(config.sta.password, request->credential, request->credential_size);
    if (request->security == WIFI_SECURITY_OPEN) {
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else if (request->security == WIFI_SECURITY_WPA3_SAE) {
        config.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    } else {
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK)
        return -1;
    backend.disconnect_locally = false;
    return esp_wifi_connect() == ESP_OK ? 0 : -1;
}

int wlh_wifi_backend_disconnect(void *context) {
    (void)context;
    if (!backend.driver_started)
        return -1;
    backend.disconnect_locally = true;
    return esp_wifi_disconnect() == ESP_OK ? 0 : -1;
}

void wlh_wifi_backend_ethernet_tx(
    void *context, const uint8_t *frame, size_t size
) {
    (void)context;
    if (!backend.connected || frame == NULL || size == 0u || size > 1518u)
        return;
    /* esp_wifi_internal_tx copies the frame into the Wi-Fi driver queue. */
    (void)esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, (uint16_t)size);
}
