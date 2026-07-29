#include "ota_backend.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#define OTA_SLOTS 4u
#define OTA_CHUNK 4048u
#define OTA_CMD_DEPTH 4u

typedef enum { CMD_BEGIN, CMD_FINALIZE, CMD_ABORT, CMD_ACTIVATE } cmd_kind_t;
typedef struct {
    cmd_kind_t kind; uint32_t operation_id; uint32_t transfer_id;
    uint64_t image_size; uint64_t bytes_sent; bool reboot; uint8_t sha256[32];
} ota_cmd_t;
typedef struct { uint32_t transfer_id; uint64_t offset; size_t size; uint8_t data[OTA_CHUNK]; } ota_slot_t;
typedef struct {
    wlh_coproc_t *coproc; TaskHandle_t task; QueueHandle_t commands, free_slots, pending_slots;
    const esp_partition_t *partition; esp_ota_handle_t handle; bool active;
    uint32_t transfer_id; uint64_t bytes; uint8_t expected_sha[32]; mbedtls_sha256_context sha;
    ota_slot_t slots[OTA_SLOTS];
} ota_backend_t;
static ota_backend_t backend;
static const char *TAG = "wlh-ota";

static int queue_command(const ota_cmd_t *cmd) {
    return backend.task != NULL && xQueueSend(backend.commands, cmd, 0) == pdTRUE ? 0 : -1;
}
static int backend_begin(void *ctx, uint32_t op, uint32_t id, const wlh_coproc_ota_begin_params_t *p) {
    ota_cmd_t cmd; (void)ctx; if (p == NULL || p->image_size > UINT32_MAX) return -1;
    memset(&cmd, 0, sizeof(cmd)); cmd.kind = CMD_BEGIN; cmd.operation_id = op; cmd.transfer_id = id;
    cmd.image_size = p->image_size; memcpy(cmd.sha256, p->sha256, sizeof(cmd.sha256)); return queue_command(&cmd);
}
static int backend_write(void *ctx, uint32_t id, uint64_t offset, const uint8_t *data, size_t size) {
    uint8_t index; ota_slot_t *slot; (void)ctx;
    if (data == NULL || size == 0u || size > OTA_CHUNK || backend.task == NULL ||
        xQueueReceive(backend.free_slots, &index, 0) != pdTRUE) return -1;
    slot = &backend.slots[index]; slot->transfer_id = id; slot->offset = offset; slot->size = size;
    memcpy(slot->data, data, size);
    if (xQueueSend(backend.pending_slots, &index, 0) != pdTRUE) { (void)xQueueSend(backend.free_slots, &index, 0); return -1; }
    (void)xTaskNotifyGive(backend.task); return 0;
}
static int backend_finalize(void *ctx, uint32_t op, uint32_t id, uint64_t bytes) {
    ota_cmd_t cmd; (void)ctx; memset(&cmd, 0, sizeof(cmd)); cmd.kind = CMD_FINALIZE; cmd.operation_id = op; cmd.transfer_id = id; cmd.bytes_sent = bytes; return queue_command(&cmd);
}
static int backend_abort(void *ctx, uint32_t op, uint32_t id) {
    ota_cmd_t cmd; (void)ctx; memset(&cmd, 0, sizeof(cmd)); cmd.kind = CMD_ABORT; cmd.operation_id = op; cmd.transfer_id = id; return queue_command(&cmd);
}
static int backend_activate(void *ctx, uint32_t op, uint32_t id, bool reboot) {
    ota_cmd_t cmd; (void)ctx; memset(&cmd, 0, sizeof(cmd)); cmd.kind = CMD_ACTIVATE; cmd.operation_id = op; cmd.transfer_id = id; cmd.reboot = reboot; return queue_command(&cmd);
}
wlh_coproc_ota_ops_t wlh_ota_backend_ops(void) { return (wlh_coproc_ota_ops_t){NULL, backend_begin, backend_write, backend_finalize, backend_abort, backend_activate}; }

static void clear_slots(void) { uint8_t index; while (xQueueReceive(backend.pending_slots, &index, 0) == pdTRUE) (void)xQueueSend(backend.free_slots, &index, 0); }
static void process_command(const ota_cmd_t *cmd) {
    esp_err_t err = ESP_FAIL;
    if (cmd->kind == CMD_BEGIN) {
        if (backend.active) (void)esp_ota_abort(backend.handle);
        clear_slots(); backend.partition = esp_ota_get_next_update_partition(NULL);
        if (backend.partition != NULL) err = esp_ota_begin(backend.partition, cmd->image_size, &backend.handle);
        if (err == ESP_OK) { backend.active = true; backend.transfer_id = cmd->transfer_id; backend.bytes = 0u; memcpy(backend.expected_sha, cmd->sha256, 32u); mbedtls_sha256_init(&backend.sha); mbedtls_sha256_starts(&backend.sha, 0); }
        (void)wlh_coproc_ota_begin_complete(backend.coproc, cmd->operation_id, err == ESP_OK ? 0 : -1);
    } else if (cmd->kind == CMD_FINALIZE) {
        uint8_t actual[32]; err = (backend.active && cmd->transfer_id == backend.transfer_id && cmd->bytes_sent == backend.bytes) ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) { mbedtls_sha256_finish(&backend.sha, actual); if (memcmp(actual, backend.expected_sha, sizeof(actual)) != 0) err = ESP_FAIL; }
        if (err == ESP_OK) err = esp_ota_end(backend.handle);
        if (err != ESP_OK && backend.active) (void)esp_ota_abort(backend.handle);
        backend.active = false; (void)wlh_coproc_ota_finalize_complete(backend.coproc, cmd->operation_id, err == ESP_OK ? 0 : -1);
    } else if (cmd->kind == CMD_ABORT) {
        if (backend.active) { (void)esp_ota_abort(backend.handle); backend.active = false; } clear_slots();
        (void)wlh_coproc_ota_abort_complete(backend.coproc, cmd->operation_id, 0);
    } else {
        err = cmd->transfer_id == backend.transfer_id && backend.partition != NULL ? esp_ota_set_boot_partition(backend.partition) : ESP_FAIL;
        (void)wlh_coproc_ota_activate_complete(backend.coproc, cmd->operation_id, err == ESP_OK ? 0 : -1);
        if (err == ESP_OK && cmd->reboot) { vTaskDelay(pdMS_TO_TICKS(400u)); esp_restart(); }
    }
}
static void ota_task(void *arg) {
    (void)arg;
    for (;;) {
        ota_cmd_t cmd; uint8_t index;
        if (xQueueReceive(backend.commands, &cmd, pdMS_TO_TICKS(10u)) == pdTRUE) process_command(&cmd);
        while (xQueueReceive(backend.pending_slots, &index, 0) == pdTRUE) {
            ota_slot_t *slot = &backend.slots[index]; int status = -1;
            if (backend.active && slot->transfer_id == backend.transfer_id && slot->offset == backend.bytes && esp_ota_write(backend.handle, slot->data, slot->size) == ESP_OK) { mbedtls_sha256_update(&backend.sha, slot->data, slot->size); backend.bytes += slot->size; status = 0; }
            (void)wlh_coproc_ota_write_complete(backend.coproc, slot->transfer_id, backend.bytes, status);
            (void)xQueueSend(backend.free_slots, &index, 0);
        }
    }
}
int wlh_ota_backend_init(wlh_coproc_t *coproc) {
    uint8_t i; if (coproc == NULL || backend.task != NULL) return -1; memset(&backend, 0, sizeof(backend)); backend.coproc = coproc;
    backend.commands = xQueueCreate(OTA_CMD_DEPTH, sizeof(ota_cmd_t)); backend.free_slots = xQueueCreate(OTA_SLOTS, sizeof(uint8_t)); backend.pending_slots = xQueueCreate(OTA_SLOTS, sizeof(uint8_t));
    if (backend.commands == NULL || backend.free_slots == NULL || backend.pending_slots == NULL) return -1;
    for (i = 0u; i < OTA_SLOTS; ++i) (void)xQueueSend(backend.free_slots, &i, 0);
    return xTaskCreate(ota_task, "wlh-ota", 4096u, NULL, 5, &backend.task) == pdPASS ? 0 : -1;
}
void wlh_ota_backend_reset(void) { if (backend.active) (void)esp_ota_abort(backend.handle); backend.active = false; clear_slots(); }
