#include "wifi_backend.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define WLH_WIFI_SCAN_MAX_RESULTS 24u
#define WLH_WIFI_AP_MAX_CLIENTS 10u
#define WLH_WIFI_AP_DEFAULT_MAX_CLIENTS 4u
/* The pool depth is the P4's in-flight window AND the C6's TX queueing
 * delay. A 48-frame pool (72.9 KB) exceeds the P4's 64 KB lwIP TCP window,
 * so under TCP the pool can never drain and its queueing delay (RTT ~86 ms
 * at 8 Mbps) caps the sender near ~6 Mbps. 32 frames (48.6 KB) leaves the
 * window room to drive ~20 Mbps. */
#define WLH_WIFI_TX_QUEUE_DEPTH 32u
#define WLH_WIFI_MAX_ETHERNET_FRAME_SIZE 1518u

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
    bool sta_mac_valid;
    uint8_t sta_mac[6];
    bool disconnect_locally;
    bool ap_active;
    uint32_t sta_reported_session_id;
    QueueHandle_t sta_tx_free;
    QueueHandle_t sta_tx_pending;
    TaskHandle_t sta_tx_task;
} wifi_backend_t;

typedef struct wifi_tx_frame {
    uint32_t session_id;
    uint8_t channel;
    wifi_interface_t interface;
    size_t size;
    uint8_t data[WLH_WIFI_MAX_ETHERNET_FRAME_SIZE];
} wifi_tx_frame_t;

static wifi_backend_t backend;
static wifi_tx_frame_t sta_tx_pool[WLH_WIFI_TX_QUEUE_DEPTH];

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

/* A USB transport reset creates a new WL-hosted session but does not tear
 * down the ESP-IDF STA association.  ESP-IDF therefore does not emit another
 * WIFI_EVENT_STA_CONNECTED when the new host asks to connect to the same AP.
 * Keep the connected event construction in one place so that an existing
 * association can be reported again and the new host session can bring its
 * Ethernet netif up. */
static wlh_coproc_result_t report_sta_connected(
    const wifi_ap_record_t *ap_info
) {
    wlh_coproc_bss_t bss;

    if (ap_info == NULL)
        return WLH_COPROC_INVALID_ARGUMENT;
    memset(&bss, 0, sizeof(bss));
    bss.ssid = ap_info->ssid;
    bss.ssid_size = strnlen((const char *)ap_info->ssid, sizeof(ap_info->ssid));
    memcpy(bss.bssid, ap_info->bssid, sizeof(bss.bssid));
    bss.channel = ap_info->primary;
    bss.rssi_dbm = ap_info->rssi;
    bss.security = map_security(ap_info->authmode);
    if (esp_wifi_get_mac(WIFI_IF_STA, bss.interface_mac) != ESP_OK) {
        ESP_LOGE(TAG, "failed to read STA MAC after connection");
        return WLH_COPROC_BACKEND_ERROR;
    }
    memcpy(backend.sta_mac, bss.interface_mac, sizeof(backend.sta_mac));
    backend.sta_mac_valid = true;
    {
        wlh_coproc_result_t result =
            wlh_coproc_wifi_connected(backend.coproc, &bss);
        if (result == WLH_COPROC_OK && backend.coproc != NULL)
            backend.sta_reported_session_id = backend.coproc->session_id;
        return result;
    }
}

static bool sta_reported_for_current_session(void) {
    return backend.coproc != NULL && backend.coproc->session_id != 0u &&
           backend.sta_reported_session_id == backend.coproc->session_id;
}

static uint32_t sta_tx_dropped;
static uint32_t sta_tx_errors;

static bool throttled_log(uint32_t count) {
    return count <= 5u || count % 100u == 0u;
}

static bool wifi_tx_interface_active(wifi_interface_t interface) {
    return interface == WIFI_IF_AP ? backend.ap_active : backend.connected;
}

static void sta_tx_task_main(void *argument) {
    (void)argument;
    for (;;) {
        wifi_tx_frame_t *frame = NULL;
        esp_err_t result = ESP_FAIL;
        if (xQueueReceive(backend.sta_tx_pending, &frame, portMAX_DELAY) !=
                pdTRUE ||
            frame == NULL)
            continue;
        while (wifi_tx_interface_active(frame->interface)) {
            result = esp_wifi_internal_tx(
                frame->interface, frame->data, (uint16_t)frame->size
            );
            if (result != ESP_ERR_NO_MEM)
                break;
            /* The Wi-Fi driver applies backpressure by exhausting its TX
             * buffers. Keep the frame in this bounded worker instead of
             * dropping it from the Core callback. */
            vTaskDelay(1u);
        }
        if (result != ESP_OK && wifi_tx_interface_active(frame->interface)) {
            ++sta_tx_errors;
            if (throttled_log(sta_tx_errors))
                ESP_LOGW(
                    TAG,
                    "datapath: wifi tx failed #%lu: %s",
                    (unsigned long)sta_tx_errors,
                    esp_err_to_name(result)
                );
        }
        (void)wlh_coproc_ethernet_rx_complete(
            backend.coproc,
            frame->session_id,
            frame->channel,
            1u,
            result == ESP_OK ? 0 : (int)result
        );
        configASSERT(
            xQueueSend(backend.sta_tx_free, &frame, portMAX_DELAY) == pdTRUE
        );
    }
}

static bool sta_frame_for_host(const uint8_t *frame, uint16_t length) {
    static const uint8_t broadcast[6] = {
        0xffu,
        0xffu,
        0xffu,
        0xffu,
        0xffu,
        0xffu,
    };

    return length >= sizeof(broadcast) &&
           (memcmp(frame, broadcast, sizeof(broadcast)) == 0 ||
            (backend.sta_mac_valid &&
             memcmp(frame, backend.sta_mac, sizeof(backend.sta_mac)) == 0));
}

static esp_err_t sta_rx_callback(void *buffer, uint16_t length, void *eb) {
    /* Runs on the Wi-Fi task. The Core copies the frame into its bounded
     * queue, so the driver-owned RX buffer can be released immediately after
     * wlh_coproc_ethernet_sta_send returns. */
    if (backend.coproc != NULL && buffer != NULL &&
        sta_frame_for_host(buffer, length)) {
        (void)wlh_coproc_ethernet_sta_send(
            backend.coproc, buffer, (size_t)length
        );
    }
    if (eb != NULL)
        esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

static esp_err_t ap_rx_callback(void *buffer, uint16_t length, void *eb) {
    if (backend.coproc != NULL && buffer != NULL && length > 0u) {
        (void)wlh_coproc_ethernet_ap_send(
            backend.coproc, buffer, (size_t)length
        );
    }
    if (eb != NULL)
        esp_wifi_internal_free_rx_buffer(eb);
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
        wlh_coproc_result_t result;
        backend.connected = true;
        ESP_LOGI(
            TAG,
            "event received: STA_CONNECTED ssid=%.*s channel=%u",
            (int)event->ssid_len,
            (const char *)event->ssid,
            (unsigned)event->channel
        );
        if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
            /* The SDK has already told us that association succeeded.  This
             * should be transient, but do not manufacture a partial link
             * event because the Host needs its STA MAC to bring netif up. */
            ESP_LOGE(TAG, "failed to read AP record after connection");
            result = WLH_COPROC_BACKEND_ERROR;
        } else {
            result = report_sta_connected(&ap_info);
        }
        ESP_LOGI(TAG, "event complete: STA_CONNECTED report=%d", (int)result);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *event = data;
        bool locally_initiated = backend.disconnect_locally;
        wlh_coproc_result_t result;
        backend.disconnect_locally = false;
        backend.connected = false;
        backend.sta_mac_valid = false;
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
    size_t index;
    if (coproc == NULL)
        return -1;
    memset(&backend, 0, sizeof(backend));
    backend.coproc = coproc;
    backend.sta_tx_free =
        xQueueCreate(WLH_WIFI_TX_QUEUE_DEPTH, sizeof(wifi_tx_frame_t *));
    backend.sta_tx_pending =
        xQueueCreate(WLH_WIFI_TX_QUEUE_DEPTH, sizeof(wifi_tx_frame_t *));
    if (backend.sta_tx_free == NULL || backend.sta_tx_pending == NULL)
        return -1;
    for (index = 0u; index < WLH_WIFI_TX_QUEUE_DEPTH; ++index) {
        wifi_tx_frame_t *frame = &sta_tx_pool[index];
        if (xQueueSend(backend.sta_tx_free, &frame, 0) != pdTRUE)
            return -1;
    }
    if (xTaskCreate(
            sta_tx_task_main,
            "wlh-wifi-tx",
            4096u,
            NULL,
            7,
            &backend.sta_tx_task
        ) != pdPASS)
        return -1;
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
        wifi_ap_record_t ap_info;
        wlh_coproc_result_t initialized;

        /* The driver and STA association survive a transport reset.  A
         * freshly negotiated Host session still needs the link event, even
         * though ESP-IDF will not re-emit STA_CONNECTED. */
        initialized =
            wlh_coproc_wifi_initialized(backend.coproc, operation_id, 0);
        if (initialized != WLH_COPROC_OK)
            return -1;
        if (backend.connected && !sta_reported_for_current_session() &&
            esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            wlh_coproc_result_t report = report_sta_connected(&ap_info);
            ESP_LOGI(
                TAG,
                "resynchronized existing STA association report=%d",
                (int)report
            );
            if (report != WLH_COPROC_OK)
                return -1;
        }
        return 0;
    }
    if (interface_flags == 0u || interface_flags > 3u)
        return -1;
    mode = (interface_flags & 3u) == 1u   ? WIFI_MODE_STA
           : (interface_flags & 3u) == 2u ? WIFI_MODE_AP
                                          : WIFI_MODE_APSTA;
    if (esp_wifi_set_mode(mode) != ESP_OK)
        return -1;
    backend.interface_flags = interface_flags;
    /* Record the operation before esp_wifi_start(): the STA_START event is
       delivered from the event-loop task and can run before esp_wifi_start()
       returns, so the handler must already see it. */
    backend.initialize_operation_id = operation_id;
    if (esp_wifi_start() != ESP_OK) {
        backend.initialize_operation_id = 0u;
        return -1;
    }
    /* Hosted Ethernet is an always-on data path. Modem power save adds
     * hundreds of milliseconds of burst latency to TCP ACKs and can let SDIO
     * RX traffic accumulate behind beacon sleep intervals. Match the
     * esp-hosted-mcu coprocessor profile and keep the radio awake while the
     * WL-hosted Wi-Fi service is active. */
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        backend.initialize_operation_id = 0u;
        (void)esp_wifi_stop();
        return -1;
    }
    backend.driver_started = true;
    (void)esp_wifi_internal_reg_rxcb(WIFI_IF_STA, sta_rx_callback);
    ESP_LOGI(
        TAG,
        "Wi-Fi started mode=%s flags=%lu",
        mode == WIFI_MODE_STA  ? "STA"
        : mode == WIFI_MODE_AP ? "AP"
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
    if (backend.connected) {
        wifi_ap_record_t ap_info;

        /* Replaying CONNECT after a Host/USB session reset must restore the
         * state event even though the STA never disconnected.  Only do this
         * for the AP requested by the new Host; a different SSID still goes
         * through ESP-IDF's normal reconnect path. */
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
            strnlen((const char *)ap_info.ssid, sizeof(ap_info.ssid)) ==
                request->ssid_size &&
            memcmp(ap_info.ssid, request->ssid, request->ssid_size) == 0) {
            if (!sta_reported_for_current_session()) {
                wlh_coproc_result_t report = report_sta_connected(&ap_info);
                ESP_LOGI(
                    TAG,
                    "resynchronized existing STA for CONNECT report=%d",
                    (int)report
                );
                return report == WLH_COPROC_OK ? 0 : -1;
            }
            return 0;
        }
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
    if (esp_wifi_internal_reg_rxcb(WIFI_IF_AP, ap_rx_callback) != ESP_OK)
        return -1;
    backend.ap_active = true;
    {
        wlh_coproc_bss_t ap;
        wifi_config_t actual;
        memset(&ap, 0, sizeof(ap));
        memset(&actual, 0, sizeof(actual));
        ap.ssid = request->ssid;
        ap.ssid_size = request->ssid_size;
        ap.security = request->security;
        ap.channel = config.ap.channel == 0u ? 1u : config.ap.channel;
        if (esp_wifi_get_config(WIFI_IF_AP, &actual) == ESP_OK)
            ap.channel = actual.ap.channel;
        if (esp_wifi_get_mac(WIFI_IF_AP, ap.interface_mac) != ESP_OK)
            return -1;
        memcpy(ap.bssid, ap.interface_mac, sizeof(ap.bssid));
        if (wlh_coproc_wifi_ap_started(backend.coproc, &ap) != WLH_COPROC_OK)
            return -1;
    }
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
    (void)esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
    (void)wlh_coproc_wifi_ap_stopped(
        backend.coproc, DISCONNECT_REASON_LOCAL_REQUEST, true
    );
    ESP_LOGI(TAG, "AP stopped");
    return 0;
}

static wlh_coproc_ethernet_rx_result_t submit_ethernet_tx(
    uint32_t session_id,
    uint8_t channel,
    wifi_interface_t interface,
    const uint8_t *frame,
    size_t size,
    bool active
) {
    wifi_tx_frame_t *pending = NULL;
    if (!active || frame == NULL || size == 0u ||
        size > WLH_WIFI_MAX_ETHERNET_FRAME_SIZE ||
        xQueueReceive(backend.sta_tx_free, &pending, 0) != pdTRUE) {
        ++sta_tx_dropped;
        if (throttled_log(sta_tx_dropped))
            ESP_LOGW(
                TAG,
                "datapath: host->wifi drop #%lu connected=%d size=%u",
                (unsigned long)sta_tx_dropped,
                (int)active,
                (unsigned)size
            );
        return WLH_COPROC_ETHERNET_RX_REJECTED;
    }
    pending->session_id = session_id;
    pending->channel = channel;
    pending->interface = interface;
    pending->size = size;
    memcpy(pending->data, frame, size);
    if (xQueueSend(backend.sta_tx_pending, &pending, 0) != pdTRUE) {
        ++sta_tx_dropped;
        configASSERT(xQueueSend(backend.sta_tx_free, &pending, 0) == pdTRUE);
        return WLH_COPROC_ETHERNET_RX_REJECTED;
    }
    return WLH_COPROC_ETHERNET_RX_PENDING;
}

wlh_coproc_ethernet_rx_result_t wlh_wifi_backend_ethernet_ap_tx(
    void *context,
    uint32_t session_id,
    uint8_t channel,
    const uint8_t *frame,
    size_t size
) {
    (void)context;
    return submit_ethernet_tx(
        session_id, channel, WIFI_IF_AP, frame, size, backend.ap_active
    );
}

wlh_coproc_ethernet_rx_result_t wlh_wifi_backend_ethernet_tx(
    void *context,
    uint32_t session_id,
    uint8_t channel,
    const uint8_t *frame,
    size_t size
) {
    (void)context;
    return submit_ethernet_tx(
        session_id, channel, WIFI_IF_STA, frame, size, backend.connected
    );
}

uint32_t wlh_wifi_backend_ethernet_rx_capacity(void) {
    return WLH_WIFI_TX_QUEUE_DEPTH;
}
