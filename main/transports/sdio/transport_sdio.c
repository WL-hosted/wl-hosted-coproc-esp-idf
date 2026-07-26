#include "transport.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/sdio_slave.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wlh/protocol/wire.h"

#define WLH_SDIO_MAX_FRAME_SIZE 4092u
#define WLH_SDIO_RESET_EVENT (1u << 0)
#define WLH_SDIO_LINK_RESET_INTERRUPT 0u
#define WLH_SDIO_TX_WAIT_MS 1000u

static const char *TAG = "wlh-sdio";

typedef struct tx_job {
    uint8_t *frame;
    size_t size;
    wlh_coproc_tx_complete_fn completion;
    void *completion_context;
} tx_job_t;

typedef struct tx_pending {
    bool in_use;
    uint8_t *dma_frame;
    tx_job_t job;
} tx_pending_t;

typedef struct sdio_transport {
    wlh_coproc_t *coproc;
    size_t max_frame_size;
    wlh_transport_reset_fn on_reset;
    void *reset_context;
    QueueHandle_t tx_queue;
    EventGroupHandle_t events;
    SemaphoreHandle_t state_lock;
    SemaphoreHandle_t tx_window;
    TaskHandle_t tx_task;
    TaskHandle_t tx_done_task;
    TaskHandle_t rx_task;
    TaskHandle_t reset_task;
    atomic_bool resetting;
    tx_pending_t pending[CONFIG_WLH_SDIO_TX_QUEUE_DEPTH];
    sdio_slave_buf_handle_t rx_handles[CONFIG_WLH_SDIO_RX_BUFFER_COUNT];
} sdio_transport_t;

static sdio_transport_t transport;
static DMA_ATTR uint8_t
    rx_buffers[CONFIG_WLH_SDIO_RX_BUFFER_COUNT][WLH_SDIO_MAX_FRAME_SIZE];

static sdio_slave_timing_t configured_timing(void) {
#if CONFIG_WLH_SDIO_TIMING_PSEND_PSAMPLE
    return SDIO_SLAVE_TIMING_PSEND_PSAMPLE;
#elif CONFIG_WLH_SDIO_TIMING_NSEND_PSAMPLE
    return SDIO_SLAVE_TIMING_NSEND_PSAMPLE;
#elif CONFIG_WLH_SDIO_TIMING_PSEND_NSAMPLE
    return SDIO_SLAVE_TIMING_PSEND_NSAMPLE;
#else
    return SDIO_SLAVE_TIMING_NSEND_NSAMPLE;
#endif
}

static uint32_t configured_flags(void) {
    uint32_t flags = 0u;
#if CONFIG_WLH_SDIO_DEFAULT_SPEED
    flags |= SDIO_SLAVE_FLAG_DEFAULT_SPEED;
#endif
#if CONFIG_WLH_SDIO_INTERNAL_PULLUP
    flags |= SDIO_SLAVE_FLAG_INTERNAL_PULLUP;
#endif
    return flags;
}

static void complete_job(tx_job_t *job, int status) {
    if (job->completion != NULL)
        job->completion(job->completion_context, job->frame, job->size, status);
}

static void flush_tx_queue(int status) {
    tx_job_t job;
    while (xQueueReceive(transport.tx_queue, &job, 0) == pdTRUE)
        complete_job(&job, status);
}

static tx_pending_t *find_free_pending(void) {
    size_t index;
    for (index = 0u; index < CONFIG_WLH_SDIO_TX_QUEUE_DEPTH; ++index) {
        if (!transport.pending[index].in_use)
            return &transport.pending[index];
    }
    return NULL;
}

int wlh_transport_submit_tx(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
) {
    tx_job_t job;
    (void)context;
    if (frame == NULL || size == 0u || size > WLH_SDIO_MAX_FRAME_SIZE ||
        atomic_load(&transport.resetting))
        return -1;
    job.frame = frame;
    job.size = size;
    job.completion = completion;
    job.completion_context = completion_context;
    if (xQueueSend(transport.tx_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full: rejecting %u bytes", (unsigned)size);
        return -1;
    }
    return 0;
}

static void tx_task_main(void *argument) {
    tx_job_t job;
    (void)argument;
    for (;;) {
        uint8_t *dma_frame;
        tx_pending_t *pending;
        esp_err_t result;

        if (xQueueReceive(transport.tx_queue, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (xSemaphoreTake(transport.tx_window, portMAX_DELAY) != pdTRUE) {
            complete_job(&job, -1);
            continue;
        }
        if (atomic_load(&transport.resetting)) {
            complete_job(&job, -1);
            xSemaphoreGive(transport.tx_window);
            continue;
        }
        dma_frame =
            heap_caps_malloc(job.size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (dma_frame == NULL) {
            ESP_LOGE(
                TAG,
                "unable to allocate %u-byte DMA TX buffer",
                (unsigned)job.size
            );
            complete_job(&job, -1);
            xSemaphoreGive(transport.tx_window);
            continue;
        }
        memcpy(dma_frame, job.frame, job.size);

        xSemaphoreTake(transport.state_lock, portMAX_DELAY);
        pending = find_free_pending();
        if (pending == NULL || atomic_load(&transport.resetting)) {
            xSemaphoreGive(transport.state_lock);
            heap_caps_free(dma_frame);
            complete_job(&job, -1);
            xSemaphoreGive(transport.tx_window);
            continue;
        }
        pending->in_use = true;
        pending->dma_frame = dma_frame;
        pending->job = job;
        result = sdio_slave_send_queue(
            dma_frame, job.size, pending, pdMS_TO_TICKS(WLH_SDIO_TX_WAIT_MS)
        );
        if (result != ESP_OK) {
            memset(pending, 0, sizeof(*pending));
            xSemaphoreGive(transport.state_lock);
            heap_caps_free(dma_frame);
            ESP_LOGW(TAG, "SDIO TX queue failed: %s", esp_err_to_name(result));
            complete_job(&job, -1);
            xSemaphoreGive(transport.tx_window);
            continue;
        }
        xSemaphoreGive(transport.state_lock);
    }
}

static void tx_done_task_main(void *argument) {
    (void)argument;
    for (;;) {
        void *finished = NULL;
        tx_job_t job;
        uint8_t *dma_frame = NULL;
        tx_pending_t *pending;

        if (sdio_slave_send_get_finished(&finished, portMAX_DELAY) != ESP_OK)
            continue;
        pending = finished;
        if (pending == NULL)
            continue;
        memset(&job, 0, sizeof(job));
        xSemaphoreTake(transport.state_lock, portMAX_DELAY);
        if (pending->in_use) {
            job = pending->job;
            dma_frame = pending->dma_frame;
            memset(pending, 0, sizeof(*pending));
        }
        xSemaphoreGive(transport.state_lock);
        if (dma_frame != NULL) {
            heap_caps_free(dma_frame);
            complete_job(&job, 0);
            xSemaphoreGive(transport.tx_window);
        }
    }
}

static void rx_task_main(void *argument) {
    (void)argument;
    for (;;) {
        sdio_slave_buf_handle_t handle = NULL;
        uint8_t *frame = NULL;
        size_t size = 0u;
        esp_err_t result =
            sdio_slave_recv(&handle, &frame, &size, portMAX_DELAY);
        if (result != ESP_OK)
            continue;
        if (frame == NULL || size < WLH_FRAME_HEADER_SIZE ||
            size > transport.max_frame_size ||
            wlh_frame_validate(frame, size, transport.max_frame_size) !=
                WLH_WIRE_OK) {
            ESP_LOGW(
                TAG,
                "dropping invalid SDIO transaction: %u bytes",
                (unsigned)size
            );
        } else {
            wlh_coproc_result_t core_result;
            /*
             * Match esp-hosted-mcu's ownership model: do not reload this
             * slave RX buffer until the bounded Core queue has accepted the
             * frame. In particular, CreditUpdate and heartbeat frames must
             * not be dropped behind a burst of Wi-Fi Ethernet jobs.
             */
            do {
                core_result =
                    wlh_coproc_on_frame(transport.coproc, frame, size);
                if (core_result == WLH_COPROC_BACKEND_ERROR)
                    vTaskDelay(pdMS_TO_TICKS(1u));
            } while (core_result == WLH_COPROC_BACKEND_ERROR &&
                     !atomic_load(&transport.resetting));
            if (core_result != WLH_COPROC_OK) {
                ESP_LOGW(
                    TAG, "core rejected SDIO frame: result=%d", (int)core_result
                );
            }
        }
        if (handle != NULL && sdio_slave_recv_load_buf(handle) != ESP_OK)
            ESP_LOGE(TAG, "failed to reload SDIO RX buffer");
    }
}

static size_t take_pending_jobs(tx_job_t *jobs, uint8_t **dma_frames) {
    size_t count = 0u;
    size_t index;

    for (index = 0u; index < CONFIG_WLH_SDIO_TX_QUEUE_DEPTH; ++index) {
        if (!transport.pending[index].in_use)
            continue;
        jobs[count] = transport.pending[index].job;
        dma_frames[count] = transport.pending[index].dma_frame;
        count++;
        memset(&transport.pending[index], 0, sizeof(transport.pending[index]));
    }
    return count;
}

static void reset_task_main(void *argument) {
    tx_job_t jobs[CONFIG_WLH_SDIO_TX_QUEUE_DEPTH];
    uint8_t *dma_frames[CONFIG_WLH_SDIO_TX_QUEUE_DEPTH];
    (void)argument;
    for (;;) {
        size_t count;
        size_t index;

        xEventGroupWaitBits(
            transport.events,
            WLH_SDIO_RESET_EVENT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );
        atomic_store(&transport.resetting, true);
        ESP_LOGW(TAG, "host requested SDIO link reset");

        xSemaphoreTake(transport.state_lock, portMAX_DELAY);
        sdio_slave_stop();
        (void)sdio_slave_reset();
        count = take_pending_jobs(jobs, dma_frames);
        sdio_slave_set_host_intena(
            SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET | SDIO_SLAVE_HOSTINT_BIT0
        );
        if (sdio_slave_start() != ESP_OK)
            ESP_LOGE(TAG, "failed to restart SDIO slave");
        xSemaphoreGive(transport.state_lock);

        flush_tx_queue(-1);
        for (index = 0u; index < count; ++index) {
            heap_caps_free(dma_frames[index]);
            complete_job(&jobs[index], -1);
            /* Return the window token held by each reclaimed in-flight
               job so the TX path restarts with a full window. */
            xSemaphoreGive(transport.tx_window);
        }
        atomic_store(&transport.resetting, false);
        if (transport.on_reset != NULL)
            transport.on_reset(transport.reset_context);
    }
}

IRAM_ATTR static void sdio_event_callback(uint8_t interrupt_number) {
    BaseType_t woken = pdFALSE;
    if (interrupt_number != WLH_SDIO_LINK_RESET_INTERRUPT)
        return;
    xEventGroupSetBitsFromISR(transport.events, WLH_SDIO_RESET_EVENT, &woken);
    portYIELD_FROM_ISR(woken);
}

static int initialize_driver(void) {
    sdio_slave_config_t config = {
        .timing = configured_timing(),
        .sending_mode = SDIO_SLAVE_SEND_PACKET,
        .send_queue_size = CONFIG_WLH_SDIO_TX_QUEUE_DEPTH,
        .recv_buffer_size = WLH_SDIO_MAX_FRAME_SIZE,
        .event_cb = sdio_event_callback,
        .flags = configured_flags(),
    };
    size_t index;

    if (sdio_slave_initialize(&config) != ESP_OK)
        return -1;
    for (index = 0u; index < CONFIG_WLH_SDIO_RX_BUFFER_COUNT; ++index) {
        transport.rx_handles[index] =
            sdio_slave_recv_register_buf(rx_buffers[index]);
        if (transport.rx_handles[index] == NULL ||
            sdio_slave_recv_load_buf(transport.rx_handles[index]) != ESP_OK) {
            ESP_LOGE(
                TAG, "failed to register SDIO RX buffer %u", (unsigned)index
            );
            sdio_slave_deinit();
            return -1;
        }
    }
    sdio_slave_set_host_intena(
        SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET | SDIO_SLAVE_HOSTINT_BIT0
    );
    return sdio_slave_start() == ESP_OK ? 0 : -1;
}

int wlh_transport_start(const wlh_transport_config_t *config) {
    if (config == NULL || config->coproc == NULL ||
        config->max_frame_size != WLH_SDIO_MAX_FRAME_SIZE)
        return -1;
    memset(&transport, 0, sizeof(transport));
    transport.coproc = config->coproc;
    transport.max_frame_size = config->max_frame_size;
    transport.on_reset = config->on_reset;
    transport.reset_context = config->reset_context;
    transport.tx_queue =
        xQueueCreate(CONFIG_WLH_SDIO_TX_QUEUE_DEPTH, sizeof(tx_job_t));
    transport.events = xEventGroupCreate();
    transport.state_lock = xSemaphoreCreateMutex();
    transport.tx_window = xSemaphoreCreateCounting(
        CONFIG_WLH_SDIO_TX_QUEUE_DEPTH, CONFIG_WLH_SDIO_TX_QUEUE_DEPTH
    );
    if (transport.tx_queue == NULL || transport.events == NULL ||
        transport.state_lock == NULL || transport.tx_window == NULL ||
        initialize_driver() != 0) {
        ESP_LOGE(TAG, "SDIO transport initialization failed");
        return -1;
    }
    if (xTaskCreate(
            tx_task_main, "wlh-sdio-tx", 4096u, NULL, 8, &transport.tx_task
        ) != pdPASS ||
        xTaskCreate(
            tx_done_task_main,
            "wlh-sdio-done",
            4096u,
            NULL,
            9,
            &transport.tx_done_task
        ) != pdPASS ||
        xTaskCreate(
            rx_task_main, "wlh-sdio-rx", 4096u, NULL, 8, &transport.rx_task
        ) != pdPASS ||
        xTaskCreate(
            reset_task_main,
            "wlh-sdio-reset",
            4096u,
            NULL,
            9,
            &transport.reset_task
        ) != pdPASS) {
        ESP_LOGE(TAG, "SDIO task creation failed");
        return -1;
    }
    ESP_LOGI(
        TAG,
        "SDIO slave started: max_frame=%u txq=%u rx_buffers=%u",
        (unsigned)transport.max_frame_size,
        (unsigned)CONFIG_WLH_SDIO_TX_QUEUE_DEPTH,
        (unsigned)CONFIG_WLH_SDIO_RX_BUFFER_COUNT
    );
    return 0;
}

size_t wlh_transport_max_frame_size(void) {
    return WLH_SDIO_MAX_FRAME_SIZE;
}
