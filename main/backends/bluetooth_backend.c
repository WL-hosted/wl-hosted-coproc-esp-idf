#include "bluetooth_backend.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define WLH_BT_SLOT_COUNT CONFIG_WLH_BLUETOOTH_HCI_QUEUE_DEPTH
#define WLH_BT_MAX_PACKET CONFIG_WLH_BLUETOOTH_MAX_HCI_PACKET
#define WLH_BT_CMD_QUEUE_DEPTH 4u
#define WLH_BT_RESET_TIMEOUT_MS 3000u

/* Reason 1 is WLH_COPROC_BLUETOOTH_REASON_MALFORMED_HCI, owned by the core. */
#define WLH_BT_REASON_RX_OVERFLOW 2u
#define WLH_BT_REASON_CONTROLLER_FAILURE 3u

/* Controller->host HCI classification. LE advertising/scan reports arrive
 * from the air at a rate no host-side flow control can bound, so they are
 * best-effort: shedding them under RX pool pressure is normal, not fatal.
 * All other HCI (command status/complete, connection events, ACL) is
 * reliable and must never be dropped silently. */
#define HCI_H4_EVT 0x04u
/* Slots kept in reserve for reliable HCI so an advertising-report flood can
 * never starve a critical frame. At least one; a quarter of the pool. */
#define WLH_BT_RX_ADV_RESERVE                                                  \
    ((WLH_BT_SLOT_COUNT / 4u) > 0u ? (WLH_BT_SLOT_COUNT / 4u) : 1u)

#define EV_CMD (1u << 0)
#define EV_TX (1u << 1)
#define EV_RX (1u << 2)
#define EV_VHCI_SEND (1u << 3)
#define EV_CREDIT (1u << 4)
#define EV_FATAL (1u << 5)
#define EV_RESET (1u << 6)
#define EV_ALL                                                                 \
    (EV_CMD | EV_TX | EV_RX | EV_VHCI_SEND | EV_CREDIT | EV_FATAL | EV_RESET)

static const char *TAG = "wlh-bt";

typedef enum bt_cmd_kind {
    BT_CMD_INITIALIZE,
    BT_CMD_ENABLE,
    BT_CMD_DISABLE,
    BT_CMD_DEINITIALIZE,
    BT_CMD_GET_INFO
} bt_cmd_kind_t;

typedef struct bt_cmd {
    bt_cmd_kind_t kind;
    uint32_t operation_id;
    bool release_memory;
} bt_cmd_t;

typedef struct hci_slot {
    uint16_t size; /* H4 type byte + payload */
    uint8_t data[1u + WLH_BT_MAX_PACKET];
} hci_slot_t;

typedef struct bluetooth_backend {
    wlh_coproc_t *coproc;
    TaskHandle_t task;
    EventGroupHandle_t events;
    QueueHandle_t commands;
    QueueHandle_t tx_free;
    QueueHandle_t tx_pending;
    QueueHandle_t rx_free;
    QueueHandle_t rx_pending;
    SemaphoreHandle_t reset_done;
    volatile bool resetting;
    volatile bool fatal_pending;
    uint32_t fatal_reason;
    uint32_t rx_overflows;
    uint32_t rx_rejected;
    uint32_t rx_adv_dropped;
} bluetooth_backend_t;

static bluetooth_backend_t backend;
static hci_slot_t tx_slots[WLH_BT_SLOT_COUNT];
static hci_slot_t rx_slots[WLH_BT_SLOT_COUNT];

static void report_fatal(uint32_t reason) {
    if (!backend.fatal_pending) {
        backend.fatal_reason = reason;
        backend.fatal_pending = true;
    }
    xEventGroupSetBits(backend.events, EV_FATAL);
}

/* Runs on the controller task when the controller can accept a packet. */
static void vhci_send_available(void) {
    xEventGroupSetBits(backend.events, EV_VHCI_SEND);
}

/* True for best-effort LE advertising/scan reports, which the radio produces
 * at a rate no host-side flow control can bound. `packet` starts with its H4
 * type byte. */
static bool hci_is_droppable_adv_report(const uint8_t *packet, uint16_t size) {
    return size >= 1u && packet[0] == HCI_H4_EVT &&
           wlh_hci_event_is_adv_report(packet + 1u, (size_t)size - 1u);
}

/* Runs on the controller task. `data` starts with the H4 type byte and is
 * only valid during the call, so it is copied into a fixed RX slot.
 *
 * Advertising reports are best-effort: under RX pool pressure they are
 * dropped here, before the Core assigns a wire sequence number, so no gap
 * appears on the reliable HCI channel and the controller keeps running. A
 * reserve keeps room for reliable HCI (command status/complete, connection
 * events, ACL); losing one of those is a genuine backpressure contract
 * violation and stays fatal. */
static int vhci_receive(uint8_t *data, uint16_t length) {
    uint8_t index;
    bool droppable;
    if (data == NULL || length < 2u ||
        (size_t)length > 1u + (size_t)WLH_BT_MAX_PACKET) {
        report_fatal(WLH_BT_REASON_CONTROLLER_FAILURE);
        return 0;
    }
    droppable = hci_is_droppable_adv_report(data, length);
    if (droppable &&
        uxQueueMessagesWaiting(backend.rx_free) <= WLH_BT_RX_ADV_RESERVE) {
        ++backend.rx_adv_dropped;
        return 0;
    }
    if (xQueueReceive(backend.rx_free, &index, 0) != pdTRUE) {
        if (droppable) {
            ++backend.rx_adv_dropped;
            return 0;
        }
        ++backend.rx_overflows;
        report_fatal(WLH_BT_REASON_RX_OVERFLOW);
        return 0;
    }
    rx_slots[index].size = length;
    memcpy(rx_slots[index].data, data, length);
    (void)xQueueSend(backend.rx_pending, &index, 0);
    xEventGroupSetBits(backend.events, EV_RX);
    return 0;
}

static const esp_vhci_host_callback_t vhci_callbacks = {
    vhci_send_available,
    vhci_receive,
};

static int submit_command(const bt_cmd_t *command) {
    if (backend.task == NULL || backend.resetting)
        return -1;
    if (xQueueSend(backend.commands, command, 0) != pdTRUE)
        return -1;
    xEventGroupSetBits(backend.events, EV_CMD);
    return 0;
}

static int backend_initialize(
    void *context, uint32_t operation_id, uint32_t feature_flags
) {
    bt_cmd_t command = {BT_CMD_INITIALIZE, operation_id, false};
    (void)context;
    (void)feature_flags;
    return submit_command(&command);
}

static int backend_enable(
    void *context, uint32_t operation_id, uint32_t mode_flags
) {
    bt_cmd_t command = {BT_CMD_ENABLE, operation_id, false};
    (void)context;
    (void)mode_flags;
    return submit_command(&command);
}

static int backend_disable(void *context, uint32_t operation_id) {
    bt_cmd_t command = {BT_CMD_DISABLE, operation_id, false};
    (void)context;
    return submit_command(&command);
}

static int backend_deinitialize(
    void *context, uint32_t operation_id, bool release_memory
) {
    bt_cmd_t command = {BT_CMD_DEINITIALIZE, operation_id, release_memory};
    (void)context;
    return submit_command(&command);
}

static int backend_get_info(void *context, uint32_t operation_id) {
    bt_cmd_t command = {BT_CMD_GET_INFO, operation_id, false};
    (void)context;
    return submit_command(&command);
}

/* Runs on the Core task. Copies the packet into a fixed TX slot; the wlh-bt
 * task pushes it into the VHCI when the controller is ready. A nonzero
 * return is fatal for session HCI, so only genuine faults reject. */
static int backend_hci_send(
    void *context, uint8_t h4_type, const uint8_t *payload, size_t payload_size
) {
    uint8_t index;
    (void)context;
    if (backend.task == NULL || backend.resetting)
        return -1;
    if (payload == NULL || payload_size > (size_t)WLH_BT_MAX_PACKET)
        return -1;
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED)
        return -1;
    if (xQueueReceive(backend.tx_free, &index, 0) != pdTRUE) {
        ESP_LOGE(TAG, "host->controller HCI slots exhausted");
        return -1;
    }
    tx_slots[index].data[0] = h4_type;
    memcpy(&tx_slots[index].data[1], payload, payload_size);
    tx_slots[index].size = (uint16_t)(payload_size + 1u);
    (void)xQueueSend(backend.tx_pending, &index, 0);
    xEventGroupSetBits(backend.events, EV_TX);
    return 0;
}

/* Runs on the Core task with core locks held: only wake the wlh-bt task. */
static void backend_hci_tx_ready(void *context) {
    (void)context;
    xEventGroupSetBits(backend.events, EV_CREDIT);
}

wlh_coproc_bluetooth_ops_t wlh_bluetooth_backend_ops(void) {
    return (wlh_coproc_bluetooth_ops_t){NULL,
                                        backend_initialize,
                                        backend_enable,
                                        backend_disable,
                                        backend_deinitialize,
                                        backend_get_info,
                                        backend_hci_send,
                                        backend_hci_tx_ready};
}

static int controller_initialize(void) {
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t result = esp_bt_controller_init(&config);
        if (result != ESP_OK) {
            ESP_LOGE(
                TAG, "controller init failed: %s", esp_err_to_name(result)
            );
            return -1;
        }
    }
    return 0;
}

static int controller_enable(void) {
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    esp_err_t result;
    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED)
        return 0;
    if (status != ESP_BT_CONTROLLER_STATUS_INITED)
        return -1;
    result = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(result));
        return -1;
    }
    if (esp_vhci_host_register_callback(&vhci_callbacks) != ESP_OK) {
        ESP_LOGE(TAG, "VHCI callback registration failed");
        return -1;
    }
    return 0;
}

static int controller_disable(void) {
    esp_err_t result;
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED)
        return 0;
    result = esp_bt_controller_disable();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "controller disable failed: %s", esp_err_to_name(result));
        return -1;
    }
    return 0;
}

static int controller_deinitialize(void) {
    esp_err_t result;
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_INITED)
        return 0;
    result = esp_bt_controller_deinit();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "controller deinit failed: %s", esp_err_to_name(result));
        return -1;
    }
    return 0;
}

static void flush_ring(QueueHandle_t pending, QueueHandle_t free_queue) {
    uint8_t index;
    while (xQueueReceive(pending, &index, 0) == pdTRUE)
        (void)xQueueSend(free_queue, &index, 0);
}

static void run_get_info(uint32_t operation_id) {
    wlh_coproc_bluetooth_info_t info;
    memset(&info, 0, sizeof(info));
    if (esp_read_mac(info.public_address, ESP_MAC_BT) == ESP_OK)
        info.has_public_address = true;
    /* hci_version, manufacturer_id and feature_bits stay 0: no public
     * ESP-IDF API reports them reliably in controller-only mode. */
    info.max_hci_packet = WLH_BT_MAX_PACKET < WLH_COPROC_MAX_HCI_PACKET
                              ? WLH_BT_MAX_PACKET
                              : WLH_COPROC_MAX_HCI_PACKET;
    (void)wlh_coproc_bluetooth_info_result(
        backend.coproc, operation_id, 0, &info
    );
}

static void run_command(const bt_cmd_t *command) {
    int result;
    switch (command->kind) {
    case BT_CMD_INITIALIZE:
        result = controller_initialize();
        break;
    case BT_CMD_ENABLE:
        result = controller_enable();
        break;
    case BT_CMD_DISABLE:
        result = controller_disable();
        if (result == 0) {
            flush_ring(backend.tx_pending, backend.tx_free);
            flush_ring(backend.rx_pending, backend.rx_free);
        }
        break;
    case BT_CMD_DEINITIALIZE:
        /* Releasing controller memory is one-way on ESP32 targets and would
         * require a chip reset to reinitialize, so it is not supported. */
        if (command->release_memory) {
            result = -1;
            break;
        }
        result = controller_disable();
        if (result == 0)
            result = controller_deinitialize();
        if (result == 0) {
            flush_ring(backend.tx_pending, backend.tx_free);
            flush_ring(backend.rx_pending, backend.rx_free);
        }
        break;
    case BT_CMD_GET_INFO:
        run_get_info(command->operation_id);
        return;
    default:
        result = -1;
        break;
    }
    (void)wlh_coproc_bluetooth_operation_complete(
        backend.coproc, command->operation_id, result
    );
}

static void drain_tx(void) {
    uint8_t index;
    while (xQueuePeek(backend.tx_pending, &index, 0) == pdTRUE) {
        if (!esp_vhci_host_check_send_available())
            return; /* vhci_send_available wakes the task again. */
        esp_vhci_host_send_packet(tx_slots[index].data, tx_slots[index].size);
        (void)xQueueReceive(backend.tx_pending, &index, 0);
        (void)xQueueSend(backend.tx_free, &index, 0);
    }
}

static void drain_rx(void) {
    uint8_t index;
    while (xQueuePeek(backend.rx_pending, &index, 0) == pdTRUE) {
        wlh_coproc_result_t result = wlh_coproc_bluetooth_hci_send(
            backend.coproc,
            rx_slots[index].data[0],
            &rx_slots[index].data[1],
            (size_t)rx_slots[index].size - 1u
        );
        if (result == WLH_COPROC_NO_CREDIT)
            return; /* Keep the slot; hci_tx_ready retries the drain. */
        (void)xQueueReceive(backend.rx_pending, &index, 0);
        (void)xQueueSend(backend.rx_free, &index, 0);
        if (result != WLH_COPROC_OK) {
            ++backend.rx_rejected;
            ESP_LOGW(
                TAG,
                "controller->host HCI rejected by core: %d (total %lu)",
                (int)result,
                (unsigned long)backend.rx_rejected
            );
        }
    }
}

static void run_reset(void) {
    bt_cmd_t command;
    if (controller_disable() != 0 || controller_deinitialize() != 0) {
        ESP_LOGE(
            TAG,
            "controller did not shut down cleanly; Bluetooth stays "
            "unavailable until chip reset"
        );
    }
    while (xQueueReceive(backend.commands, &command, 0) == pdTRUE) {
    }
    flush_ring(backend.tx_pending, backend.tx_free);
    flush_ring(backend.rx_pending, backend.rx_free);
    xEventGroupClearBits(backend.events, EV_ALL & ~EV_RESET);
    backend.fatal_pending = false;
    backend.resetting = false;
    (void)xSemaphoreGive(backend.reset_done);
}

static void bluetooth_task(void *argument) {
    (void)argument;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            backend.events, EV_ALL, pdTRUE, pdFALSE, portMAX_DELAY
        );
        if ((bits & EV_RESET) != 0u) {
            run_reset();
            continue;
        }
        if ((bits & EV_FATAL) != 0u && backend.fatal_pending) {
            uint32_t reason = backend.fatal_reason;
            backend.fatal_pending = false;
            ESP_LOGE(
                TAG, "fatal Bluetooth error, reason=%lu", (unsigned long)reason
            );
            (void)wlh_coproc_bluetooth_fatal_error(backend.coproc, reason);
        }
        if ((bits & EV_CMD) != 0u) {
            bt_cmd_t command;
            while (xQueueReceive(backend.commands, &command, 0) == pdTRUE)
                run_command(&command);
        }
        drain_tx();
        drain_rx();
    }
}

int wlh_bluetooth_backend_init(wlh_coproc_t *coproc) {
    uint8_t index;
    if (coproc == NULL || backend.task != NULL)
        return -1;
    memset(&backend, 0, sizeof(backend));
    backend.coproc = coproc;
    backend.events = xEventGroupCreate();
    backend.commands = xQueueCreate(WLH_BT_CMD_QUEUE_DEPTH, sizeof(bt_cmd_t));
    backend.tx_free = xQueueCreate(WLH_BT_SLOT_COUNT, sizeof(uint8_t));
    backend.tx_pending = xQueueCreate(WLH_BT_SLOT_COUNT, sizeof(uint8_t));
    backend.rx_free = xQueueCreate(WLH_BT_SLOT_COUNT, sizeof(uint8_t));
    backend.rx_pending = xQueueCreate(WLH_BT_SLOT_COUNT, sizeof(uint8_t));
    backend.reset_done = xSemaphoreCreateBinary();
    if (backend.events == NULL || backend.commands == NULL ||
        backend.tx_free == NULL || backend.tx_pending == NULL ||
        backend.rx_free == NULL || backend.rx_pending == NULL ||
        backend.reset_done == NULL) {
        ESP_LOGE(TAG, "backend resource allocation failed");
        return -1;
    }
    for (index = 0; index < WLH_BT_SLOT_COUNT; ++index) {
        (void)xQueueSend(backend.tx_free, &index, 0);
        (void)xQueueSend(backend.rx_free, &index, 0);
    }
    if (xTaskCreate(bluetooth_task, "wlh-bt", 4096u, NULL, 7, &backend.task) !=
        pdPASS) {
        ESP_LOGE(TAG, "wlh-bt task creation failed");
        backend.task = NULL;
        return -1;
    }
    return 0;
}

void wlh_bluetooth_backend_reset(void) {
    if (backend.task == NULL)
        return;
    backend.resetting = true;
    xEventGroupSetBits(backend.events, EV_RESET);
    if (xSemaphoreTake(
            backend.reset_done, pdMS_TO_TICKS(WLH_BT_RESET_TIMEOUT_MS)
        ) != pdTRUE) {
        ESP_LOGE(TAG, "bluetooth reset timed out; controller state unknown");
        backend.resetting = false;
    }
}
