#include "user_passthrough.h"

#include "esp_log.h"

#define USER_FLAG_EXPECT_RESULT 0x01u

static const char *TAG = "wlh-user";

static wlh_coproc_t *coproc_instance;

/* wlh_coproc_user_message_fn. Runs on the Core task; keep it nonblocking.
 * The demo endpoint echoes the payload back as a RESULT event when the host
 * sets EXPECT_RESULT. */
static int on_user_message(
    void *context, const wlh_coproc_user_message_t *message
) {
    (void)context;
    if (message == NULL)
        return -1;
    ESP_LOGI(
        TAG,
        "user message endpoint=%u type=%u flags=0x%x request=%u bytes=%u",
        (unsigned)message->endpoint_id,
        (unsigned)message->message_type,
        (unsigned)message->flags,
        (unsigned)message->request_id,
        (unsigned)message->payload_size
    );
    ESP_LOG_BUFFER_HEX(TAG, message->payload, message->payload_size);
    if ((message->flags & USER_FLAG_EXPECT_RESULT) != 0u &&
        coproc_instance != NULL) {
        (void)wlh_coproc_user_message_result(
            coproc_instance,
            message->endpoint_id,
            message->message_type,
            message->request_id,
            0,
            message->payload,
            message->payload_size
        );
    }
    return 0;
}

wlh_coproc_user_passthrough_ops_t wlh_user_passthrough_ops(
    wlh_coproc_t *coproc
) {
    wlh_coproc_user_passthrough_ops_t ops = {NULL, on_user_message};
    coproc_instance = coproc;
    return ops;
}
