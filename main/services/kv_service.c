#include "kv_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WLH_KV_NAMESPACE "wlh_kv"

/* NVS entry names are capped at NVS_KEY_NAME_MAX_SIZE (16, terminator
 * included) while the protocol allows 64-byte keys. Every record is therefore
 * stored under a 15-character hash of its key, as a blob that begins with the
 * full key:
 *
 *   blob = <full key> '\0' <value>
 *
 * A read compares the embedded key, so a hash collision reports NOT_FOUND
 * instead of returning another key's value. */
#define WLH_KV_ENTRY_NAME_SIZE 16u
#define WLH_KV_BLOB_MAX_SIZE                                                   \
    (WLH_COPROC_KV_MAX_KEY_SIZE + 1u + WLH_COPROC_KV_MAX_VALUE_SIZE)

static const char *TAG = "wlh-kv";

static void entry_name(const char *key, char out[WLH_KV_ENTRY_NAME_SIZE]) {
    /* FNV-1a, 64-bit. */
    uint64_t hash = 1469598103934665603ULL;
    size_t index;

    for (index = 0u; key[index] != '\0'; ++index) {
        hash ^= (uint64_t)(uint8_t)key[index];
        hash *= 1099511628211ULL;
    }
    /* 15 hex digits keep the terminator inside the NVS bound; the embedded key
     * check below covers the discarded nibble. */
    (void)snprintf(
        out,
        WLH_KV_ENTRY_NAME_SIZE,
        "%015llx",
        (unsigned long long)(hash & 0xfffffffffffffffULL)
    );
}

static int status_for(esp_err_t error) {
    switch (error) {
    case ESP_OK:
        return WLH_COPROC_SERVICE_OK;
    case ESP_ERR_NVS_NOT_FOUND:
        return WLH_COPROC_SERVICE_NOT_FOUND;
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
    case ESP_ERR_NVS_PAGE_FULL:
    case ESP_ERR_NVS_VALUE_TOO_LONG:
        return WLH_COPROC_SERVICE_NO_SPACE;
    case ESP_ERR_NVS_INVALID_NAME:
    case ESP_ERR_NVS_INVALID_LENGTH:
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;
    case ESP_ERR_NVS_READ_ONLY:
        return WLH_COPROC_SERVICE_INVALID_STATE;
    default:
        return WLH_COPROC_SERVICE_INTERNAL;
    }
}

static int open_namespace(nvs_open_mode_t mode, nvs_handle_t *handle) {
    esp_err_t error = nvs_open(WLH_KV_NAMESPACE, mode, handle);

    if (error != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(error));
        return status_for(error);
    }
    return WLH_COPROC_SERVICE_OK;
}

/* Loads the record for `key` and returns the offset of its value within blob,
 * or a service status on failure. */
static int load_blob(
    nvs_handle_t handle,
    const char *key,
    const char *name,
    uint8_t *blob,
    size_t *blob_size,
    size_t *value_offset
) {
    size_t key_size = strlen(key);
    esp_err_t error;

    *blob_size = WLH_KV_BLOB_MAX_SIZE;
    error = nvs_get_blob(handle, name, blob, blob_size);
    if (error != ESP_OK)
        return status_for(error);
    /* Reject a record whose embedded key differs: the name is only a hash. */
    if (*blob_size <= key_size || blob[key_size] != '\0' ||
        memcmp(blob, key, key_size) != 0)
        return WLH_COPROC_SERVICE_NOT_FOUND;
    *value_offset = key_size + 1u;
    return WLH_COPROC_SERVICE_OK;
}

static int read_key(
    void *context,
    const char *key,
    char *value,
    size_t value_capacity,
    size_t *value_size
) {
    uint8_t blob[WLH_KV_BLOB_MAX_SIZE];
    char name[WLH_KV_ENTRY_NAME_SIZE];
    nvs_handle_t handle;
    size_t blob_size = 0u;
    size_t offset = 0u;
    size_t stored_size;
    int status;

    (void)context;
    if (key == NULL || value == NULL || value_size == NULL)
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;
    entry_name(key, name);
    status = open_namespace(NVS_READONLY, &handle);
    if (status != WLH_COPROC_SERVICE_OK)
        return status;

    status = load_blob(handle, key, name, blob, &blob_size, &offset);
    nvs_close(handle);
    if (status != WLH_COPROC_SERVICE_OK)
        return status;

    stored_size = blob_size - offset;
    if (stored_size >= value_capacity)
        return WLH_COPROC_SERVICE_INTERNAL;
    memcpy(value, blob + offset, stored_size);
    value[stored_size] = '\0';
    *value_size = stored_size;
    return WLH_COPROC_SERVICE_OK;
}

static int write_key(
    void *context, const char *key, const char *value, size_t value_size
) {
    uint8_t blob[WLH_KV_BLOB_MAX_SIZE];
    char name[WLH_KV_ENTRY_NAME_SIZE];
    nvs_handle_t handle;
    size_t key_size;
    esp_err_t error;
    int status;

    (void)context;
    if (key == NULL || value == NULL)
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;
    key_size = strlen(key);
    if (key_size > WLH_COPROC_KV_MAX_KEY_SIZE ||
        value_size > WLH_COPROC_KV_MAX_VALUE_SIZE)
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;

    memcpy(blob, key, key_size);
    blob[key_size] = '\0';
    memcpy(blob + key_size + 1u, value, value_size);

    entry_name(key, name);
    status = open_namespace(NVS_READWRITE, &handle);
    if (status != WLH_COPROC_SERVICE_OK)
        return status;

    error = nvs_set_blob(handle, name, blob, key_size + 1u + value_size);
    /* Commit before returning so a host that gets OK can rely on the value
     * surviving a reset. */
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "write failed: %s", esp_err_to_name(error));
        return status_for(error);
    }
    return WLH_COPROC_SERVICE_OK;
}

static int erase_key(void *context, const char *key) {
    uint8_t blob[WLH_KV_BLOB_MAX_SIZE];
    char name[WLH_KV_ENTRY_NAME_SIZE];
    nvs_handle_t handle;
    size_t blob_size = 0u;
    size_t offset = 0u;
    esp_err_t error;
    int status;

    (void)context;
    if (key == NULL)
        return WLH_COPROC_SERVICE_INVALID_ARGUMENT;
    entry_name(key, name);
    status = open_namespace(NVS_READWRITE, &handle);
    if (status != WLH_COPROC_SERVICE_OK)
        return status;

    /* Confirm the record belongs to this key before erasing, so a colliding
     * hash cannot destroy an unrelated entry. */
    status = load_blob(handle, key, name, blob, &blob_size, &offset);
    if (status != WLH_COPROC_SERVICE_OK) {
        nvs_close(handle);
        return status;
    }

    error = nvs_erase_key(handle, name);
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "erase failed: %s", esp_err_to_name(error));
        return status_for(error);
    }
    return WLH_COPROC_SERVICE_OK;
}

wlh_coproc_kv_ops_t wlh_kv_ops(void) {
    wlh_coproc_kv_ops_t ops = {NULL, read_key, write_key, erase_key};
    return ops;
}
