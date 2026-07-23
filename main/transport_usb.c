#include "transport_usb.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usbd_core.h"

#define WLH_USB_VID 0x303au
#define WLH_USB_PID 0x8201u
#define WLH_USB_MAX_POWER_MA 500u

#define WLH_USB_EP_OUT 0x01u
#define WLH_USB_EP_IN 0x81u
#define WLH_USB_EP_MPS 64u

#define WLH_USB_BUS_ID 0u
/* Arm one maximum-size packet at a time. If a larger buffer is armed,
 * CherryUSB waits for a short packet before completing the read; a wire
 * frame whose size is exactly a multiple of 64 bytes would then remain
 * buffered indefinitely. rx_feed() already reassembles the byte stream. */
#define WLH_USB_OUT_CHUNK WLH_USB_EP_MPS
#define WLH_USB_RX_RING_SIZE (2u * 4096u + 512u)
#define WLH_USB_TX_QUEUE_DEPTH 8u
#define WLH_USB_TX_TIMEOUT_MS 2000u
#define WLH_USB_CONFIGURED_BIT (1u << 0)
#define WLH_USB_RESET_BIT (1u << 1)

#define FRAME_HEADER_SIZE 24u
#define FRAME_MAGIC_BYTE0 0x57u
#define FRAME_MAGIC_BYTE1 0x4cu
#define FRAME_PROTOCOL_MAJOR 1u
#define FRAME_FLAGS_MASK 0x03u

static const char *TAG = "wlh-usb";

typedef struct tx_job {
    uint8_t *frame;
    size_t size;
    wlh_coproc_tx_complete_fn completion;
    void *completion_context;
} tx_job_t;

typedef struct usb_transport {
    wlh_coproc_t *coproc;
    size_t max_frame_size;
    wlh_usb_bus_reset_fn on_bus_reset;
    void *bus_reset_context;

    QueueHandle_t tx_queue;
    SemaphoreHandle_t tx_done;
    EventGroupHandle_t events;
    RingbufHandle_t rx_ring;
    StaticRingbuffer_t rx_ring_struct;
    uint8_t rx_ring_storage[WLH_USB_RX_RING_SIZE];
    TaskHandle_t tx_task;
    TaskHandle_t rx_task;
    atomic_bool stopping;
    /* Set on the first failed transfer; cleared when the host configures
     * the device again. Prevents restart cascades while the stack is
     * already re-enumerating or the cable is unplugged. */
    atomic_bool restart_pending;
    uint32_t rx_overruns;

    uint8_t rx_frame[FRAME_HEADER_SIZE + 4096u];
    size_t rx_frame_length;
} usb_transport_t;

static usb_transport_t transport;

static DMA_ATTR uint8_t out_chunk[WLH_USB_OUT_CHUNK];
static char serial_string[13];
static const char langid_string[] = {0x09, 0x04};

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

#define USB_CONFIG_SIZE (9 + 9 + 7 + 7)

static const uint8_t device_descriptor[] = {USB_DEVICE_DESCRIPTOR_INIT(
    USB_2_0, 0x00, 0x00, 0x00, WLH_USB_VID, WLH_USB_PID, 0x0100, 0x01
)};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(
        USB_CONFIG_SIZE,
        0x01,
        0x01,
        USB_CONFIG_BUS_POWERED,
        WLH_USB_MAX_POWER_MA / 2u
    ),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(
        WLH_USB_EP_OUT, USB_ENDPOINT_TYPE_BULK, WLH_USB_EP_MPS, 0x00
    ),
    USB_ENDPOINT_DESCRIPTOR_INIT(
        WLH_USB_EP_IN, USB_ENDPOINT_TYPE_BULK, WLH_USB_EP_MPS, 0x00
    ),
};

static const uint8_t *device_descriptor_callback(uint8_t speed) {
    (void)speed;
    return device_descriptor;
}
static const uint8_t *config_descriptor_callback(uint8_t speed) {
    (void)speed;
    return config_descriptor;
}
static const char *string_descriptor_callback(uint8_t speed, uint8_t index) {
    static const char *strings[] = {
        langid_string,
        "WL-hosted",
        "WL-hosted ESP32-S3 Coprocessor",
        serial_string,
    };
    (void)speed;
    if (index >= sizeof(strings) / sizeof(strings[0]))
        return NULL;
    return strings[index];
}

static const struct usb_descriptor usb_descriptors = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

/* ------------------------------------------------------------------ */
/* RX: bulk OUT -> ring buffer -> reassembly task -> core              */
/* ------------------------------------------------------------------ */

static void out_endpoint_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    BaseType_t woken = pdFALSE;
    (void)ep;
    if (nbytes != 0u && !atomic_load(&transport.stopping)) {
        if (xRingbufferSendFromISR(
                transport.rx_ring, out_chunk, nbytes, &woken
            ) != pdTRUE) {
            /* Ring overrun: the stream is desynchronized; the link layer
             * recovers via session timeout and re-Hello. */
            transport.rx_overruns++;
        }
    }
    (void)usbd_ep_start_read(
        busid, WLH_USB_EP_OUT, out_chunk, sizeof(out_chunk)
    );
    portYIELD_FROM_ISR(woken);
}

static void in_endpoint_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    BaseType_t woken = pdFALSE;
    (void)busid;
    (void)ep;
    (void)nbytes;
    if (transport.tx_done != NULL)
        xSemaphoreGiveFromISR(transport.tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

static struct usbd_endpoint out_endpoint = {
    .ep_addr = WLH_USB_EP_OUT,
    .ep_cb = out_endpoint_callback,
};
static struct usbd_endpoint in_endpoint = {
    .ep_addr = WLH_USB_EP_IN,
    .ep_cb = in_endpoint_callback,
};

static struct usbd_interface vendor_interface = {0};

static bool frame_header_plausible(const uint8_t *header) {
    return header[0] == FRAME_MAGIC_BYTE0 && header[1] == FRAME_MAGIC_BYTE1 &&
           header[2] == FRAME_PROTOCOL_MAJOR &&
           header[3] == FRAME_HEADER_SIZE &&
           (header[5] & (uint8_t)~FRAME_FLAGS_MASK) == 0u;
}

static void rx_consume(size_t count) {
    transport.rx_frame_length -= count;
    if (transport.rx_frame_length != 0u) {
        memmove(
            transport.rx_frame,
            transport.rx_frame + count,
            transport.rx_frame_length
        );
    }
}

static void rx_feed(const uint8_t *data, size_t size) {
    if (transport.rx_frame_length + size > sizeof(transport.rx_frame)) {
        transport.rx_frame_length = 0u;
        return;
    }
    memcpy(transport.rx_frame + transport.rx_frame_length, data, size);
    transport.rx_frame_length += size;

    while (transport.rx_frame_length >= FRAME_HEADER_SIZE) {
        size_t frame_size;
        if (!frame_header_plausible(transport.rx_frame)) {
            rx_consume(1u);
            continue;
        }
        frame_size = FRAME_HEADER_SIZE + (size_t)transport.rx_frame[6] +
                     ((size_t)transport.rx_frame[7] << 8);
        if (frame_size > transport.max_frame_size) {
            rx_consume(1u);
            continue;
        }
        if (transport.rx_frame_length < frame_size)
            break;
        wlh_coproc_result_t result = wlh_coproc_on_frame(
            transport.coproc, transport.rx_frame, frame_size
        );
        if (result != WLH_COPROC_OK) {
            ESP_LOGW(
                TAG,
                "RX frame rejected: bytes=%u result=%d",
                (unsigned)frame_size,
                (int)result
            );
        }
        rx_consume(frame_size);
    }
}

static void rx_task_main(void *argument) {
    (void)argument;
    for (;;) {
        size_t size;
        uint8_t *chunk =
            xRingbufferReceive(transport.rx_ring, &size, pdMS_TO_TICKS(100u));
        if (chunk == NULL)
            continue;
        rx_feed(chunk, size);
        vRingbufferReturnItem(transport.rx_ring, chunk);
    }
}

/* ------------------------------------------------------------------ */
/* TX: core submit -> queue -> task -> bulk IN                         */
/* ------------------------------------------------------------------ */

int wlh_usb_transport_submit_tx(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
) {
    tx_job_t job;
    (void)context;
    job.frame = frame;
    job.size = size;
    job.completion = completion;
    job.completion_context = completion_context;
    if (atomic_load(&transport.stopping))
        return -1;
    if (xQueueSend(transport.tx_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full: dropping %u bytes", (unsigned)size);
        return -1;
    }
    return 0;
}

static void flush_tx_queue(int status) {
    tx_job_t job;
    while (xQueueReceive(transport.tx_queue, &job, 0) == pdTRUE) {
        if (job.frame == NULL)
            continue;
        job.completion(job.completion_context, job.frame, job.size, status);
    }
}

static void usb_event_handler(uint8_t busid, uint8_t event);

static void usb_stack_register(void) {
    usbd_desc_register(WLH_USB_BUS_ID, &usb_descriptors);
    usbd_add_interface(WLH_USB_BUS_ID, &vendor_interface);
    usbd_add_endpoint(WLH_USB_BUS_ID, &out_endpoint);
    usbd_add_endpoint(WLH_USB_BUS_ID, &in_endpoint);
}

/* A bulk IN transfer that never completes means the host went away without
 * a bus reset (e.g. the host process closed libusb while a write was
 * armed). Detaching and re-attaching the controller forces a fresh
 * enumeration, which drives the standard RESET/CONFIGURED recovery path. */
static void usb_stack_restart(void) {
    ESP_LOGW(
        TAG,
        "restarting usb device stack (queue=%u, overruns=%lu)",
        (unsigned)uxQueueMessagesWaiting(transport.tx_queue),
        (unsigned long)transport.rx_overruns
    );
    (void)usbd_deinitialize(WLH_USB_BUS_ID);
    vTaskDelay(pdMS_TO_TICKS(50u));
    usb_stack_register();
    if (usbd_initialize(WLH_USB_BUS_ID, ESP_USBD_BASE, usb_event_handler) != 0)
        ESP_LOGE(TAG, "usb stack restart failed");
}

static void tx_task_main(void *argument) {
    tx_job_t job;
    (void)argument;
    for (;;) {
        int status = 0;
        EventBits_t bits = xEventGroupWaitBits(
            transport.events,
            WLH_USB_RESET_BIT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(100u)
        );
        if ((bits & WLH_USB_RESET_BIT) != 0u) {
            /* Bus reset/re-enumeration: queued frames belong to the dead
             * session; complete them as failed in task context. */
            flush_tx_queue(-1);
            continue;
        }
        if (xQueueReceive(transport.tx_queue, &job, 0) != pdTRUE)
            continue;
        if (job.frame == NULL)
            break;

        xEventGroupWaitBits(
            transport.events,
            WLH_USB_CONFIGURED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );
        /* Drop stale IN completions from transfers that already timed out,
         * so the wait below only observes the current transfer. */
        (void)xSemaphoreTake(transport.tx_done, 0);
        if (usbd_ep_start_write(
                WLH_USB_BUS_ID, WLH_USB_EP_IN, job.frame, job.size
            ) != 0) {
            ESP_LOGW(
                TAG, "bulk IN write rejected (%u bytes)", (unsigned)job.size
            );
            status = -1;
        } else if (xSemaphoreTake(
                       transport.tx_done, pdMS_TO_TICKS(WLH_USB_TX_TIMEOUT_MS)
                   ) != pdTRUE) {
            ESP_LOGW(
                TAG, "bulk IN transfer timed out (%u bytes)", (unsigned)job.size
            );
            status = -1;
        }
        job.completion(job.completion_context, job.frame, job.size, status);
        if (status != 0 && !atomic_exchange(&transport.restart_pending, true)) {
            /* A failed transfer means the host went away without a bus
             * reset, or the controller is wedged. Detach and re-attach to
             * force a fresh enumeration; the Core enters FAILED on the
             * completion above and restarts via the RESET event. */
            usb_stack_restart();
        }
    }
}

/* ------------------------------------------------------------------ */
/* USB device events                                                   */
/* ------------------------------------------------------------------ */

static void usb_event_handler(uint8_t busid, uint8_t event) {
    BaseType_t woken = pdFALSE;
    switch (event) {
    case USBD_EVENT_RESET:
        xEventGroupClearBitsFromISR(transport.events, WLH_USB_CONFIGURED_BIT);
        xEventGroupSetBitsFromISR(transport.events, WLH_USB_RESET_BIT, &woken);
        if (transport.on_bus_reset != NULL) {
            /* Runs in ISR context: the hook must only notify a task-safe
             * mechanism (event/queue FromISR), never the Core directly. */
            transport.on_bus_reset(transport.bus_reset_context);
        }
        break;
    case USBD_EVENT_CONFIGURED:
        atomic_store(&transport.restart_pending, false);
        xEventGroupSetBitsFromISR(
            transport.events, WLH_USB_CONFIGURED_BIT, &woken
        );
        (void)usbd_ep_start_read(
            busid, WLH_USB_EP_OUT, out_chunk, sizeof(out_chunk)
        );
        break;
    default:
        break;
    }
    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void fill_serial_string(void) {
    uint8_t mac[6];
    static const char hex[] = "0123456789abcdef";
    size_t index;
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    for (index = 0; index < sizeof(mac); ++index) {
        serial_string[index * 2u] = hex[mac[index] >> 4];
        serial_string[index * 2u + 1u] = hex[mac[index] & 0x0fu];
    }
    serial_string[12] = '\0';
}

int wlh_usb_transport_start(const wlh_usb_transport_config_t *config) {
    if (config == NULL || config->coproc == NULL ||
        config->max_frame_size > 4096u)
        return -1;

    memset(&transport, 0, sizeof(transport));
    transport.coproc = config->coproc;
    transport.max_frame_size = config->max_frame_size;
    transport.on_bus_reset = config->on_bus_reset;
    transport.bus_reset_context = config->bus_reset_context;

    transport.tx_queue = xQueueCreate(WLH_USB_TX_QUEUE_DEPTH, sizeof(tx_job_t));
    transport.tx_done = xSemaphoreCreateBinary();
    transport.events = xEventGroupCreate();
    transport.rx_ring = xRingbufferCreateStatic(
        sizeof(transport.rx_ring_storage),
        RINGBUF_TYPE_BYTEBUF,
        transport.rx_ring_storage,
        &transport.rx_ring_struct
    );
    if (transport.tx_queue == NULL || transport.tx_done == NULL ||
        transport.events == NULL || transport.rx_ring == NULL) {
        ESP_LOGE(TAG, "transport allocation failed");
        return -1;
    }

    if (xTaskCreate(
            tx_task_main, "wlh-usb-tx", 4096u, NULL, 6, &transport.tx_task
        ) != pdPASS ||
        xTaskCreate(
            rx_task_main, "wlh-usb-rx", 4096u, NULL, 6, &transport.rx_task
        ) != pdPASS) {
        ESP_LOGE(TAG, "transport task creation failed");
        return -1;
    }

    fill_serial_string();
    usb_stack_register();
    if (usbd_initialize(WLH_USB_BUS_ID, ESP_USBD_BASE, usb_event_handler) !=
        0) {
        ESP_LOGE(TAG, "usbd_initialize failed");
        return -1;
    }
    ESP_LOGI(
        TAG, "usb device started (vid=%04x pid=%04x)", WLH_USB_VID, WLH_USB_PID
    );
    return 0;
}
