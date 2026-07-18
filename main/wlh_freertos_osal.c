#include "wlh_freertos_osal.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#define FREERTOS_OSAL_DEFAULT_STACK_SIZE 4096u
#define FREERTOS_OSAL_DEFAULT_PRIORITY 5u

typedef struct freertos_osal_task_state {
    SemaphoreHandle_t done;
    wlh_osal_task_fn entry;
    void *argument;
} freertos_osal_task_state_t;

typedef struct freertos_osal_queue_state {
    StaticQueue_t buffer;
    QueueHandle_t handle;
} freertos_osal_queue_state_t;

typedef struct freertos_osal_timer_state {
    TimerHandle_t handle;
    wlh_osal_timer_fn callback;
    void *argument;
} freertos_osal_timer_state_t;

static TickType_t timeout_ticks(uint32_t timeout_ms) {
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER)
        return portMAX_DELAY;
    if (timeout_ms == WLH_OSAL_NO_WAIT)
        return 0u;
    return pdMS_TO_TICKS(timeout_ms);
}

static void task_trampoline(void *parameter) {
    freertos_osal_task_state_t *state = parameter;
    state->entry(state->argument);
    /* The task is finished: a successful join frees the state. */
    xSemaphoreGive(state->done);
    vTaskDelete(NULL);
}

static int freertos_task_create(
    void *context,
    wlh_osal_task_t *task,
    const wlh_osal_task_attributes_t *attributes,
    wlh_osal_task_fn entry,
    void *argument
) {
    freertos_osal_task_state_t *state;
    BaseType_t created;
    (void)context;

    if (task == NULL || entry == NULL)
        return -1;
    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    state->done = xSemaphoreCreateBinary();
    if (state->done == NULL) {
        free(state);
        return -1;
    }
    state->entry = entry;
    state->argument = argument;

    created = xTaskCreate(
        task_trampoline,
        attributes != NULL && attributes->name != NULL ? attributes->name
                                                       : "wlh",
        attributes != NULL && attributes->stack_size != 0u
            ? (uint32_t)attributes->stack_size
            : FREERTOS_OSAL_DEFAULT_STACK_SIZE,
        state,
        attributes != NULL ? (UBaseType_t)attributes->priority
                           : FREERTOS_OSAL_DEFAULT_PRIORITY,
        NULL
    );
    if (created != pdPASS) {
        vSemaphoreDelete(state->done);
        free(state);
        return -1;
    }
    memset(task, 0, sizeof(*task));
    task->opaque[0] = (uintptr_t)state;
    return 0;
}

static int freertos_task_join(
    void *context, wlh_osal_task_t *task, uint32_t timeout_ms
) {
    freertos_osal_task_state_t *state;
    int result = 0;
    (void)context;

    if (task == NULL || task->opaque[0] == 0u)
        return -1;
    state = (freertos_osal_task_state_t *)task->opaque[0];
    if (xSemaphoreTake(state->done, timeout_ticks(timeout_ms)) != pdTRUE) {
        /* The task is still running and owns the state; do not free it. */
        return -1;
    }
    vSemaphoreDelete(state->done);
    free(state);
    task->opaque[0] = 0u;
    return result;
}

static int freertos_mutex_create(void *context, wlh_osal_mutex_t *mutex) {
    SemaphoreHandle_t handle;
    (void)context;
    if (mutex == NULL)
        return -1;
    handle = xSemaphoreCreateMutex();
    if (handle == NULL)
        return -1;
    memset(mutex, 0, sizeof(*mutex));
    mutex->opaque[0] = (uintptr_t)handle;
    return 0;
}

static void freertos_mutex_destroy(void *context, wlh_osal_mutex_t *mutex) {
    (void)context;
    if (mutex != NULL && mutex->opaque[0] != 0u) {
        vSemaphoreDelete((SemaphoreHandle_t)mutex->opaque[0]);
        mutex->opaque[0] = 0u;
    }
}

static int freertos_mutex_lock(
    void *context, wlh_osal_mutex_t *mutex, uint32_t timeout_ms
) {
    (void)context;
    if (mutex == NULL || mutex->opaque[0] == 0u)
        return -1;
    return xSemaphoreTake(
               (SemaphoreHandle_t)mutex->opaque[0], timeout_ticks(timeout_ms)
           ) == pdTRUE
               ? 0
               : -1;
}

static void freertos_mutex_unlock(void *context, wlh_osal_mutex_t *mutex) {
    (void)context;
    if (mutex != NULL && mutex->opaque[0] != 0u)
        (void)xSemaphoreGive((SemaphoreHandle_t)mutex->opaque[0]);
}

static int freertos_semaphore_create(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    uint32_t initial_count,
    uint32_t maximum_count
) {
    SemaphoreHandle_t handle;
    (void)context;
    if (semaphore == NULL || maximum_count == 0u)
        return -1;
    handle = xSemaphoreCreateCounting(maximum_count, initial_count);
    if (handle == NULL)
        return -1;
    memset(semaphore, 0, sizeof(*semaphore));
    semaphore->opaque[0] = (uintptr_t)handle;
    return 0;
}

static void freertos_semaphore_destroy(
    void *context, wlh_osal_semaphore_t *semaphore
) {
    (void)context;
    if (semaphore != NULL && semaphore->opaque[0] != 0u) {
        vSemaphoreDelete((SemaphoreHandle_t)semaphore->opaque[0]);
        semaphore->opaque[0] = 0u;
    }
}

static int freertos_semaphore_take(
    void *context, wlh_osal_semaphore_t *semaphore, uint32_t timeout_ms
) {
    (void)context;
    if (semaphore == NULL || semaphore->opaque[0] == 0u)
        return -1;
    return xSemaphoreTake(
               (SemaphoreHandle_t)semaphore->opaque[0],
               timeout_ticks(timeout_ms)
           ) == pdTRUE
               ? 0
               : -1;
}

static int freertos_semaphore_give(
    void *context, wlh_osal_semaphore_t *semaphore
) {
    (void)context;
    if (semaphore == NULL || semaphore->opaque[0] == 0u)
        return -1;
    return xSemaphoreGive((SemaphoreHandle_t)semaphore->opaque[0]) == pdTRUE
               ? 0
               : -1;
}

static int freertos_semaphore_give_from_isr(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    bool *higher_priority_task_woken
) {
    BaseType_t woken = pdFALSE;
    BaseType_t result;
    (void)context;
    if (semaphore == NULL || semaphore->opaque[0] == 0u)
        return -1;
    result =
        xSemaphoreGiveFromISR((SemaphoreHandle_t)semaphore->opaque[0], &woken);
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = woken == pdTRUE;
    return result == pdTRUE ? 0 : -1;
}

static int freertos_event_create(void *context, wlh_osal_event_t *event) {
    EventGroupHandle_t handle;
    (void)context;
    if (event == NULL)
        return -1;
    handle = xEventGroupCreate();
    if (handle == NULL)
        return -1;
    memset(event, 0, sizeof(*event));
    event->opaque[0] = (uintptr_t)handle;
    return 0;
}

static void freertos_event_destroy(void *context, wlh_osal_event_t *event) {
    (void)context;
    if (event != NULL && event->opaque[0] != 0u) {
        vEventGroupDelete((EventGroupHandle_t)event->opaque[0]);
        event->opaque[0] = 0u;
    }
}

static int freertos_event_wait(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool wait_all,
    bool clear_on_exit,
    uint32_t timeout_ms,
    uint32_t *observed_bits
) {
    EventBits_t observed;
    bool satisfied;
    (void)context;
    if (event == NULL || event->opaque[0] == 0u || bits == 0u)
        return -1;
    observed = xEventGroupWaitBits(
        (EventGroupHandle_t)event->opaque[0],
        bits,
        clear_on_exit ? pdTRUE : pdFALSE,
        wait_all ? pdTRUE : pdFALSE,
        timeout_ticks(timeout_ms)
    );
    satisfied = wait_all ? (observed & bits) == bits : (observed & bits) != 0u;
    if (!satisfied)
        return -1;
    if (observed_bits != NULL)
        *observed_bits = observed;
    return 0;
}

static int freertos_event_set(
    void *context, wlh_osal_event_t *event, uint32_t bits
) {
    (void)context;
    if (event == NULL || event->opaque[0] == 0u)
        return -1;
    (void)xEventGroupSetBits((EventGroupHandle_t)event->opaque[0], bits);
    return 0;
}

static int freertos_event_set_from_isr(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool *higher_priority_task_woken
) {
    BaseType_t woken = pdFALSE;
    BaseType_t result;
    (void)context;
    if (event == NULL || event->opaque[0] == 0u)
        return -1;
    result = xEventGroupSetBitsFromISR(
        (EventGroupHandle_t)event->opaque[0], bits, &woken
    );
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = woken == pdTRUE;
    return result == pdTRUE ? 0 : -1;
}

static int freertos_queue_create(
    void *context,
    wlh_osal_queue_t *queue,
    void *storage,
    size_t item_size,
    size_t capacity
) {
    freertos_osal_queue_state_t *state;
    (void)context;
    if (queue == NULL || storage == NULL || item_size == 0u || capacity == 0u)
        return -1;
    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    state->handle = xQueueCreateStatic(
        capacity, item_size, (uint8_t *)storage, &state->buffer
    );
    if (state->handle == NULL) {
        free(state);
        return -1;
    }
    memset(queue, 0, sizeof(*queue));
    queue->opaque[0] = (uintptr_t)state;
    return 0;
}

static void freertos_queue_destroy(void *context, wlh_osal_queue_t *queue) {
    freertos_osal_queue_state_t *state;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return;
    state = (freertos_osal_queue_state_t *)queue->opaque[0];
    vQueueDelete(state->handle);
    free(state);
    queue->opaque[0] = 0u;
}

static int freertos_queue_send(
    void *context,
    wlh_osal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms
) {
    freertos_osal_queue_state_t *state;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return -1;
    state = (freertos_osal_queue_state_t *)queue->opaque[0];
    return xQueueSendToBack(state->handle, item, timeout_ticks(timeout_ms)) ==
                   pdTRUE
               ? 0
               : -1;
}

static int freertos_queue_send_from_isr(
    void *context,
    wlh_osal_queue_t *queue,
    const void *item,
    bool *higher_priority_task_woken
) {
    freertos_osal_queue_state_t *state;
    BaseType_t woken = pdFALSE;
    BaseType_t result;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return -1;
    state = (freertos_osal_queue_state_t *)queue->opaque[0];
    result = xQueueSendToBackFromISR(state->handle, item, &woken);
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = woken == pdTRUE;
    return result == pdTRUE ? 0 : -1;
}

static int freertos_queue_receive(
    void *context, wlh_osal_queue_t *queue, void *item, uint32_t timeout_ms
) {
    freertos_osal_queue_state_t *state;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return -1;
    state = (freertos_osal_queue_state_t *)queue->opaque[0];
    return xQueueReceive(state->handle, item, timeout_ticks(timeout_ms)) ==
                   pdTRUE
               ? 0
               : -1;
}

static void timer_trampoline(TimerHandle_t handle) {
    freertos_osal_timer_state_t *state = pvTimerGetTimerID(handle);
    state->callback(state->argument);
}

static int freertos_timer_create(
    void *context,
    wlh_osal_timer_t *timer,
    wlh_osal_timer_fn callback,
    void *argument
) {
    freertos_osal_timer_state_t *state;
    (void)context;
    if (timer == NULL || callback == NULL)
        return -1;
    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    state->callback = callback;
    state->argument = argument;
    memset(timer, 0, sizeof(*timer));
    timer->opaque[0] = (uintptr_t)state;
    return 0;
}

static void freertos_timer_destroy(void *context, wlh_osal_timer_t *timer) {
    freertos_osal_timer_state_t *state;
    (void)context;
    if (timer == NULL || timer->opaque[0] == 0u)
        return;
    state = (freertos_osal_timer_state_t *)timer->opaque[0];
    if (state->handle != NULL)
        (void)xTimerDelete(state->handle, pdMS_TO_TICKS(100u));
    free(state);
    timer->opaque[0] = 0u;
}

static int freertos_timer_start(
    void *context, wlh_osal_timer_t *timer, uint32_t period_ms, bool periodic
) {
    freertos_osal_timer_state_t *state;
    (void)context;
    if (timer == NULL || timer->opaque[0] == 0u || period_ms == 0u)
        return -1;
    state = (freertos_osal_timer_state_t *)timer->opaque[0];
    /* FreeRTOS cannot switch a timer between one-shot and periodic; recreate
     * the timer so start() fully describes the new schedule. */
    if (state->handle != NULL) {
        (void)xTimerDelete(state->handle, pdMS_TO_TICKS(100u));
        state->handle = NULL;
    }
    state->handle = xTimerCreate(
        "wlh",
        pdMS_TO_TICKS(period_ms),
        periodic ? pdTRUE : pdFALSE,
        state,
        timer_trampoline
    );
    if (state->handle == NULL)
        return -1;
    return xTimerStart(state->handle, pdMS_TO_TICKS(100u)) == pdTRUE ? 0 : -1;
}

static int freertos_timer_stop(void *context, wlh_osal_timer_t *timer) {
    freertos_osal_timer_state_t *state;
    (void)context;
    if (timer == NULL || timer->opaque[0] == 0u)
        return -1;
    state = (freertos_osal_timer_state_t *)timer->opaque[0];
    if (state->handle == NULL)
        return -1;
    return xTimerStop(state->handle, pdMS_TO_TICKS(100u)) == pdTRUE ? 0 : -1;
}

static uint64_t freertos_monotonic_time_ms(void *context) {
    (void)context;
    return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void freertos_sleep_ms(void *context, uint32_t duration_ms) {
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

static void freertos_yield(void *context) {
    (void)context;
    taskYIELD();
}

static bool freertos_in_isr(void *context) {
    (void)context;
    return xPortInIsrContext() != 0;
}

void wlh_freertos_osal_init(wlh_freertos_osal_t *osal) {
    if (osal != NULL)
        memset(osal, 0, sizeof(*osal));
}

// clang-format off
wlh_osal_ops_t wlh_freertos_osal_ops(wlh_freertos_osal_t *osal) {
    wlh_osal_ops_t ops = {
        osal,
        freertos_task_create,
        freertos_task_join,
        freertos_mutex_create,
        freertos_mutex_destroy,
        freertos_mutex_lock,
        freertos_mutex_unlock,
        freertos_semaphore_create,
        freertos_semaphore_destroy,
        freertos_semaphore_take,
        freertos_semaphore_give,
        freertos_semaphore_give_from_isr,
        freertos_event_create,
        freertos_event_destroy,
        freertos_event_wait,
        freertos_event_set,
        freertos_event_set_from_isr,
        freertos_queue_create,
        freertos_queue_destroy,
        freertos_queue_send,
        freertos_queue_send_from_isr,
        freertos_queue_receive,
        freertos_timer_create,
        freertos_timer_destroy,
        freertos_timer_start,
        freertos_timer_stop,
        freertos_monotonic_time_ms,
        freertos_sleep_ms,
        freertos_yield,
        freertos_in_isr
    };
    return ops;
}
// clang-format on
