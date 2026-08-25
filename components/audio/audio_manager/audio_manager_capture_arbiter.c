#include "audio_manager_capture_arbiter.h"

#include <string.h>

#include "audio_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CAPTURE_ARBITER_TASK_NAME       "audio_cap_arb"
#define CAPTURE_ARBITER_TASK_STACK      4096U
#define CAPTURE_ARBITER_TASK_PRIORITY   5U
#define CAPTURE_ARBITER_POLL_MS         50U
#define CAPTURE_ARBITER_LOCK_MS         100U
#define CAPTURE_ARBITER_START_MS        2000U

typedef struct {
    audio_manager_request_t request;
} capture_slot_t;

static const char *const TAG = "AUDIO_CAP_ARB";
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static TaskHandle_t s_start_waiter = NULL;
static capture_slot_t s_current = {0};
static capture_slot_t s_pending = {0};
static bool s_current_valid = false;
static bool s_pending_valid = false;
static bool s_cancel_current = false;
static bool s_preempt_current = false;
static bool s_recording_observed = false;
static audio_manager_capture_arbiter_status_t s_status = {0};

static bool take_lock(void)
{
    return (s_lock != NULL) &&
           (xSemaphoreTake(s_lock, pdMS_TO_TICKS(CAPTURE_ARBITER_LOCK_MS)) == pdTRUE);
}

static void clear_slot(capture_slot_t *slot)
{
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

static void sync_status_locked(audio_manager_capture_arbiter_state_t state,
                               esp_err_t last_error)
{
    s_status.state = state;
    s_status.current_valid = s_current_valid;
    s_status.pending_valid = s_pending_valid;
    s_status.current = s_current_valid ? s_current.request : (audio_manager_request_t){0};
    s_status.pending = s_pending_valid ? s_pending.request : (audio_manager_request_t){0};
    s_status.last_error = last_error;
}

static void promote_pending_locked(void)
{
    if (s_pending_valid) {
        s_current = s_pending;
        s_current_valid = true;
        clear_slot(&s_pending);
        s_pending_valid = false;
        s_recording_observed = false;
    }
}

static bool manager_is_external_busy(const audio_manager_status_t *manager)
{
    if (manager == NULL) {
        return false;
    }
    return ((manager->state == AUDIO_MANAGER_STATE_RECORDING) ||
            (manager->state == AUDIO_MANAGER_STATE_PROCESSING)) &&
           !s_current_valid;
}

static void capture_arbiter_task(void *arg)
{
    (void)arg;

    if (take_lock()) {
        sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_IDLE, ESP_OK);
        TaskHandle_t waiter = s_start_waiter;
        s_start_waiter = NULL;
        xSemaphoreGive(s_lock);
        if (waiter != NULL) {
            xTaskNotifyGive(waiter);
        }
    }

    for (;;) {
        audio_manager_status_t manager = {0};
        const esp_err_t status_ret = audio_manager_get_status(&manager);
        if (status_ret != ESP_OK) {
            if (take_lock()) {
                sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_ERROR, status_ret);
                ++s_status.failed_count;
                xSemaphoreGive(s_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(CAPTURE_ARBITER_POLL_MS));
            continue;
        }

        bool do_stop = false;
        bool do_start = false;
        capture_slot_t start_slot = {0};

        if (take_lock()) {
            if (s_current_valid) {
                if (manager.state == AUDIO_MANAGER_STATE_RECORDING) {
                    s_recording_observed = true;
                    if (s_cancel_current || s_preempt_current) {
                        do_stop = true;
                        sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_PREEMPTING, ESP_OK);
                    } else {
                        sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_ACTIVE, ESP_OK);
                    }
                } else if (s_recording_observed &&
                           (manager.state == AUDIO_MANAGER_STATE_PROCESSING)) {
                    sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_FINISHING, ESP_OK);
                } else if (s_recording_observed &&
                           (manager.state == AUDIO_MANAGER_STATE_IDLE)) {
                    if (s_preempt_current) {
                        ++s_status.preemption_count;
                    } else if (!s_cancel_current) {
                        ++s_status.completed_count;
                    }
                    clear_slot(&s_current);
                    s_current_valid = false;
                    s_cancel_current = false;
                    s_preempt_current = false;
                    s_recording_observed = false;
                    promote_pending_locked();
                    sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_IDLE, ESP_OK);
                }
            }

            if (!s_current_valid && s_pending_valid &&
                (manager.state == AUDIO_MANAGER_STATE_IDLE)) {
                promote_pending_locked();
            }

            if (s_current_valid && !s_recording_observed &&
                (manager.state == AUDIO_MANAGER_STATE_IDLE) &&
                !s_cancel_current && !s_preempt_current &&
                (s_status.state != AUDIO_MANAGER_CAPTURE_ARBITER_STARTING)) {
                start_slot = s_current;
                do_start = true;
                sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_STARTING, ESP_OK);
            }

            if (!s_current_valid && !s_pending_valid &&
                !manager_is_external_busy(&manager) &&
                (manager.state == AUDIO_MANAGER_STATE_IDLE)) {
                sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_IDLE, ESP_OK);
            }

            xSemaphoreGive(s_lock);
        }

        if (do_stop) {
            const esp_err_t ret = audio_manager_stop_recording();
            if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
                if (take_lock()) {
                    sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_ERROR, ret);
                    ++s_status.failed_count;
                    xSemaphoreGive(s_lock);
                }
            }
        }

        if (do_start) {
            const esp_err_t ret = audio_manager_start_recording();
            if (take_lock()) {
                if (ret == ESP_OK) {
                    sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_STARTING, ESP_OK);
                    ESP_LOGI(TAG,
                             "grant request=%u client=%s priority=%u",
                             (unsigned)start_slot.request.request_id,
                             audio_manager_client_to_string(start_slot.request.client),
                             (unsigned)start_slot.request.priority);
                } else if (ret == ESP_ERR_INVALID_STATE) {
                    /* Legacy capture may have won between our status snapshot
                     * and submission. Keep the request and retry only after the
                     * manager is genuinely IDLE again. */
                    sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_IDLE, ret);
                } else {
                    ++s_status.failed_count;
                    clear_slot(&s_current);
                    s_current_valid = false;
                    s_recording_observed = false;
                    promote_pending_locked();
                    sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_ERROR, ret);
                }
                xSemaphoreGive(s_lock);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CAPTURE_ARBITER_POLL_MS));
    }
}

esp_err_t audio_manager_capture_arbiter_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = AUDIO_MANAGER_CAPTURE_ARBITER_IDLE;
    s_status.last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t audio_manager_capture_arbiter_start(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    s_start_waiter = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(s_lock);

    if (xTaskCreate(capture_arbiter_task,
                    CAPTURE_ARBITER_TASK_NAME,
                    CAPTURE_ARBITER_TASK_STACK,
                    NULL,
                    CAPTURE_ARBITER_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        if (take_lock()) {
            s_start_waiter = NULL;
            xSemaphoreGive(s_lock);
        }
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CAPTURE_ARBITER_START_MS)) == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_manager_capture_arbiter_submit(
    const audio_manager_request_t *request)
{
    if ((s_lock == NULL) || (s_task == NULL) || (request == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((audio_manager_request_validate(request) != ESP_OK) ||
        (request->resource != AUDIO_MANAGER_RESOURCE_CAPTURE)) {
        return ESP_ERR_INVALID_ARG;
    }

    capture_slot_t incoming = {.request = *request};

    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if ((s_current_valid && s_current.request.request_id == request->request_id) ||
        (s_pending_valid && s_pending.request.request_id == request->request_id)) {
        ++s_status.rejected_count;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_current_valid) {
        s_current = incoming;
        s_current_valid = true;
        s_recording_observed = false;
        ++s_status.accepted_count;
        sync_status_locked(AUDIO_MANAGER_CAPTURE_ARBITER_IDLE, ESP_OK);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    switch (request->busy_policy) {
        case AUDIO_MANAGER_BUSY_REJECT:
            break;
        case AUDIO_MANAGER_BUSY_QUEUE:
            if (!s_pending_valid) {
                s_pending = incoming;
                s_pending_valid = true;
                ++s_status.accepted_count;
                ++s_status.queued_count;
                result = ESP_OK;
            }
            break;
        case AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY:
            if (!s_pending_valid &&
                s_current.request.interruptible &&
                (request->priority > s_current.request.priority)) {
                s_pending = incoming;
                s_pending_valid = true;
                s_preempt_current = true;
                ++s_status.accepted_count;
                ++s_status.queued_count;
                result = ESP_OK;
            }
            break;
        default:
            result = ESP_ERR_INVALID_ARG;
            break;
    }

    if (result != ESP_OK) {
        ++s_status.rejected_count;
    }
    sync_status_locked(s_status.state, s_status.last_error);
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t audio_manager_capture_arbiter_cancel(uint32_t request_id)
{
    if ((s_lock == NULL) || (s_task == NULL) || (request_id == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_pending_valid && s_pending.request.request_id == request_id) {
        clear_slot(&s_pending);
        s_pending_valid = false;
        sync_status_locked(s_status.state, ESP_OK);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    if (s_current_valid && s_current.request.request_id == request_id) {
        s_cancel_current = true;
        sync_status_locked(s_status.state, ESP_OK);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_manager_capture_arbiter_get_status(
    audio_manager_capture_arbiter_status_t *status)
{
    if ((s_lock == NULL) || (status == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *audio_manager_capture_arbiter_state_to_string(
    audio_manager_capture_arbiter_state_t state)
{
    switch (state) {
        case AUDIO_MANAGER_CAPTURE_ARBITER_UNINITIALIZED: return "UNINITIALIZED";
        case AUDIO_MANAGER_CAPTURE_ARBITER_IDLE: return "IDLE";
        case AUDIO_MANAGER_CAPTURE_ARBITER_STARTING: return "STARTING";
        case AUDIO_MANAGER_CAPTURE_ARBITER_ACTIVE: return "ACTIVE";
        case AUDIO_MANAGER_CAPTURE_ARBITER_FINISHING: return "FINISHING";
        case AUDIO_MANAGER_CAPTURE_ARBITER_PREEMPTING: return "PREEMPTING";
        case AUDIO_MANAGER_CAPTURE_ARBITER_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
