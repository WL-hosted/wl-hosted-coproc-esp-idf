#include "wifi_backend.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"

#define WLH_WIFI_SCAN_MAX_RESULTS 24u
#define WLH_WIFI_AP_MAX_CLIENTS 10u
#define WLH_WIFI_AP_DEFAULT_MAX_CLIENTS 4u

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
    uint32_t interface_flags;
    bool driver_started;
    bool connected;
    bool disconnect_locally;
    bool ap_active;
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
    ESP_LOGI(
        TAG,
        "scan done: %u AP(s) (id=%lu)",
        (unsigned)ap_count,
        (unsigned long)backend.scan_id
    );
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
    ESP_LOGI(TAG, "event complete: SCAN_DONE results=%u", (unsigned)ap_count);
    free(records);
}

static void wifi_event_handler(
    void *argument, esp_event_base_t base, int32_t id, void *data
) {
    (void)argument;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "event received: STA_START");
        if (backend.initialize_operation_id != 0u) {
            uint32_t operation_id = backend.initialize_operation_id;
            wlh_coproc_result_t result;
            backend.initialize_operation_id = 0u;
            result =
                wlh_coproc_wifi_initialized(backend.coproc, operation_id, 0);
            ESP_LOGI(
                TAG,
                "event complete: STA_START initialize=%lu report=%d",
                (unsigned long)operation_id,
                (int)result
            );
        }
        break;

    case WIFI_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "event received: SCAN_DONE");
        report_scan_done();
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *event = data;
        wifi_ap_record_t ap_info;
        wlh_coproc_bss_t bss;
        wlh_coproc_result_t result;
        memset(&bss, 0, sizeof(bss));
        backend.connected = true;
        bss.ssid = event->ssid;
        bss.ssid_size = event->ssid_len;
        memcpy(bss.bssid, event->bssid, sizeof(bss.bssid));
        if (esp_wifi_get_mac(WIFI_IF_STA, bss.interface_mac) != ESP_OK) {
            ESP_LOGE(TAG, "failed to read STA MAC after connection");
            memset(bss.interface_mac, 0, sizeof(bss.interface_mac));
        }
        bss.channel = event->channel;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            bss.rssi_dbm = ap_info.rssi;
            bss.security = map_security(ap_info.authmode);
        }
        ESP_LOGI(
            TAG,
            "event received: STA_CONNECTED ssid=%.*s channel=%u",
            (int)event->ssid_len,
            (const char *)event->ssid,
            (unsigned)event->channel
        );
        result = wlh_coproc_wifi_connected(backend.coproc, &bss);
        ESP_LOGI(TAG, "event complete: STA_CONNECTED report=%d", (int)result);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *event = data;
        bool locally_initiated = backend.disconnect_locally;
        wlh_coproc_result_t result;
        backend.disconnect_locally = false;
        backend.connected = false;
        ESP_LOGI(
            TAG,
            "event received: STA_DISCONNECTED reason=%u local=%u",
            (unsigned)event->reason,
            locally_initiated ? 1u : 0u
        );
        result = wlh_coproc_wifi_disconnected(
            backend.coproc,
            locally_initiated ? DISCONNECT_REASON_LOCAL_REQUEST
                              : map_disconnect_reason(event->reason),
            locally_initiated
        );
        ESP_LOGI(
            TAG, "event complete: STA_DISCONNECTED report=%d", (int)result
        );
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(
            TAG,
            "AP client joined: mac=%02x:%02x:%02x:%02x:%02x:%02x aid=%u",
            event->mac[0],
            event->mac[1],
            event->mac[2],
            event->mac[3],
            event->mac[4],
            event->mac[5],
            (unsigned)event->aid
        );
        (void)wlh_coproc_wifi_ap_client_joined(
            backend.coproc, event->mac, 0, event->aid
        );
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(
            TAG,
            "AP client left: mac=%02x:%02x:%02x:%02x:%02x:%02x aid=%u "
            "reason=%u",
            event->mac[0],
            event->mac[1],
            event->mac[2],
            event->mac[3],
            event->mac[4],
            event->mac[5],
            (unsigned)event->aid,
            (unsigned)event->reason
        );
        (void)wlh_coproc_wifi_ap_client_left(
            backend.coproc, event->mac, event->aid, event->reason
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
        esp_event_handler_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL
        ) != ESP_OK) {
        ESP_LOGE(TAG, "wifi backend init failed");
        return -1;
    }
    return 0;
}

int wlh_wifi_backend_initialize(
    void *context, uint32_t operation_id, uint32_t interface_flags
) {
    wifi_mode_t mode;
    (void)context;
    if (backend.driver_started) {
        /* Already up: report completion inline. */
        return wlh_coproc_wifi_initialized(backend.coproc, operation_id, 0) ==
                       WLH_COPROC_OK
                   ? 0
                   : -1;
    }
    if (interface_flags == 0u || interface_flags > 3u)
        return -1;
    mode = (interface_flags & 3u) == 1u   ? WIFI_MODE_STA
           : (interface_flags & 3u) == 2u ? WIFI_MODE_AP
                                          : WIFI_MODE_APSTA;
    if (esp_wifi_set_mode(mode) != ESP_OK)
        return -1;
    backend.interface_flags = interface_flags;
    if (esp_wifi_start() != ESP_OK)
        return -1;
    backend.initialize_operation_id = operation_id;
    backend.driver_started = true;
    (void)esp_wifi_internal_reg_rxcb(WIFI_IF_STA, sta_rx_callback);
    ESP_LOGI(
        TAG,
        "Wi-Fi started mode=%s flags=%lu",
        mode == WIFI_MODE_STA    ? "STA"
        : mode == WIFI_MODE_AP   ? "AP"
                                 : "APSTA",
        (unsigned long)interface_flags
    );
    return 0;
}

int wlh_wifi_backend_scan(void *context, uint32_t scan_id) {
    (void)context;
    if (!backend.driver_started)
        return -1;
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed (id=%lu)", (unsigned long)scan_id);
        return -1;
    }
    ESP_LOGI(TAG, "scan started (id=%lu)", (unsigned long)scan_id);
    backend.scan_id = scan_id;
    return 0;
}

int wlh_wifi_backend_connect(
    void *context, const wlh_coproc_wifi_connect_t *request
) {
    wifi_config_t config;
    esp_err_t result;
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
    } else if (request->security == WIFI_SECURITY_WPA2_WPA3_PSK) {
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    } else {
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    ESP_LOGI(
        TAG,
        "request received: CONNECT ssid=%.*s security=%lu",
        (int)request->ssid_size,
        (const char *)request->ssid,
        (unsigned long)request->security
    );
    result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "request complete: CONNECT set_config failed: %s",
            esp_err_to_name(result)
        );
        return -1;
    }
    backend.disconnect_locally = false;
    result = esp_wifi_connect();
    ESP_LOGI(
        TAG,
        "request complete: CONNECT submitted result=%s",
        esp_err_to_name(result)
    );
    return result == ESP_OK ? 0 : -1;
}

int wlh_wifi_backend_disconnect(void *context) {
    esp_err_t result;
    (void)context;
    if (!backend.driver_started)
        return -1;
    ESP_LOGI(TAG, "request received: DISCONNECT");
    backend.disconnect_locally = true;
    result = esp_wifi_disconnect();
    ESP_LOGI(
        TAG,
        "request complete: DISCONNECT submitted result=%s",
        esp_err_to_name(result)
    );
    return result == ESP_OK ? 0 : -1;
}

int wlh_wifi_backend_start_ap(
    void *context, const wlh_coproc_wifi_ap_t *request
) {
    wifi_config_t config;
    (void)context;
    if (!backend.driver_started || request == NULL ||
        request->ssid_size == 0u || request->ssid_size > 32u ||
        request->credential_size > 63u ||
        request->max_clients > WLH_WIFI_AP_MAX_CLIENTS) {
        return -1;
    }
    memset(&config, 0, sizeof(config));
    memcpy(config.ap.ssid, request->ssid, request->ssid_size);
    /* The SSID is not NUL-terminated; the length must be explicit. */
    config.ap.ssid_len = (uint8_t)request->ssid_size;
    memcpy(config.ap.password, request->credential, request->credential_size);
    if (request->credential_size == 0u ||
        request->security == WIFI_SECURITY_OPEN) {
        config.ap.authmode = WIFI_AUTH_OPEN;
    } else if (request->security == WIFI_SECURITY_WPA3_SAE) {
        config.ap.authmode = WIFI_AUTH_WPA3_PSK;
    } else {
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    /* Channel 0 lets the driver pick; in APSTA mode the SoftAP follows the
     * STA channel once the STA connects. */
    config.ap.channel = (uint8_t)request->channel;
    config.ap.max_connection =
        (uint8_t)(request->max_clients == 0u ? WLH_WIFI_AP_DEFAULT_MAX_CLIENTS
                                             : request->max_clients);
    config.ap.pmf_cfg.capable = true;
    config.ap.pmf_cfg.required = false;
    /* esp_wifi_set_config requires the AP interface to be enabled; enable it
     * when the initial request did not include AP. */
    if (!backend.ap_active) {
        wifi_mode_t current_mode;
        if (esp_wifi_get_mode(&current_mode) != ESP_OK)
            return -1;
        if (current_mode != WIFI_MODE_AP && current_mode != WIFI_MODE_APSTA) {
            if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK)
                return -1;
        }
    }
    if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK)
        return -1;
    backend.ap_active = true;
    ESP_LOGI(
        TAG,
        "AP started: ssid=%.*s channel=%u max_clients=%u",
        (int)request->ssid_size,
        (const char *)request->ssid,
        (unsigned)config.ap.channel,
        (unsigned)config.ap.max_connection
    );
    return 0;
}

int wlh_wifi_backend_stop_ap(void *context) {
    (void)context;
    if (!backend.ap_active)
        return -1;
    backend.ap_active = false;
    /* If AP was not requested during initialize, return to STA-only mode to
     * avoid keeping the SoftAP interface active. Otherwise leave the requested
     * interface resources in place. */
    if ((backend.interface_flags & 2u) == 0u) {
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
            return -1;
    }
    ESP_LOGI(TAG, "AP stopped");
    return 0;
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
