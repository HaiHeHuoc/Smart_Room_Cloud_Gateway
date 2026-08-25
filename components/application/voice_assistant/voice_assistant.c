#include "voice_assistant.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#define VOICE_ASSISTANT_TASK_NAME            "voice_assistant"
#define VOICE_ASSISTANT_TASK_STACK_BYTES     4096U
#define VOICE_ASSISTANT_TASK_PRIORITY        4U
#define VOICE_ASSISTANT_COMMAND_QUEUE_LENGTH 8U
#define VOICE_ASSISTANT_LOCK_TIMEOUT_MS      100U
#define VOICE_ASSISTANT_START_TIMEOUT_MS     2000U

typedef enum {
    VOICE_ASSISTANT_COMMAND_BEGIN_SESSION = 0,
    VOICE_ASSISTANT_COMMAND_END_SESSION,
} voice_assistant_command_type_t;

typedef struct {
    voice_assistant_command_type_t type;
    uint32_t generation;
} voice_assistant_command_t;

static const char *const TAG = "VOICE_ASSISTANT";

static SemaphoreHandle_t s_status_lock = NULL;
static QueueHandle_t s_command_queue = NULL;
static TaskHandle_t s_task_handle = NULL;
static TaskHandle_t s_start_waiter = NULL;

static voice_assistant_status_t s_status = {
    .state = VOICE_ASSISTANT_STATE_UNINITIALIZED,
    .session_generation = 0U,
    .session_active = false,
    .last_error = ESP_OK,
};

static voice_assistant_status_callback_t s_status_callback = NULL;
static void *s_status_callback_context = NULL;
static bool s_command_pending = false;

static bool voice_assistant_take_lock(void)
{
    return (s_status_lock != NULL) &&
           (xSemaphoreTake(
                s_status_lock,
                pdMS_TO_TICKS(VOICE_ASSISTANT_LOCK_TIMEOUT_MS)) == pdTRUE);
}

static void voice_assistant_publish_status(void)
{
    voice_assistant_status_t snapshot = {0};
    voice_assistant_status_callback_t callback = NULL;
    void *callback_context = NULL;

    if (!voice_assistant_take_lock()) {
        ESP_LOGW(TAG, "Status publish skipped: lock timeout");
        return;
    }

    snapshot = s_status;
    callback = s_status_callback;
    callback_context = s_status_callback_context;
    xSemaphoreGive(s_status_lock);

    /* Application code is never called while the component lock is held. */
    if (callback != NULL) {
        callback(&snapshot, callback_context);
    }
}

static void voice_assistant_set_status(
    voice_assistant_state_t state,
    bool session_active,
    esp_err_t last_error)
{
    voice_assistant_state_t previous_state = VOICE_ASSISTANT_STATE_UNINITIALIZED;
    uint32_t generation = 0U;

    if (!voice_assistant_take_lock()) {
        ESP_LOGE(TAG, "State transition dropped: lock timeout");
        return;
    }

    previous_state = s_status.state;
    s_status.state = state;
    s_status.session_active = session_active;
    s_status.last_error = last_error;
    generation = s_status.session_generation;
    s_command_pending = false;
    xSemaphoreGive(s_status_lock);

    ESP_LOGI(
        TAG,
        "state %s -> %s generation=%u active=%s error=%s",
        voice_assistant_state_to_string(previous_state),
        voice_assistant_state_to_string(state),
        (unsigned)generation,
        session_active ? "yes" : "no",
        esp_err_to_name(last_error));

    voice_assistant_publish_status();
}

static bool voice_assistant_command_is_current(
    const voice_assistant_command_t *command)
{
    if ((command == NULL) || !voice_assistant_take_lock()) {
        return false;
    }

    const uint32_t current_generation = s_status.session_generation;
    const bool current =
        (command->generation != 0U) &&
        (command->generation == current_generation);
    xSemaphoreGive(s_status_lock);

    if (!current) {
        ESP_LOGW(
            TAG,
            "drop stale command=%d generation=%u current_generation=%u",
            (int)command->type,
            (unsigned)command->generation,
            (unsigned)current_generation);
    }

    return current;
}

static void voice_assistant_task(void *argument)
{
    (void)argument;

    voice_assistant_set_status(
        VOICE_ASSISTANT_STATE_IDLE,
        false,
        ESP_OK);

    TaskHandle_t waiter = NULL;
    if (voice_assistant_take_lock()) {
        waiter = s_start_waiter;
        s_start_waiter = NULL;
        xSemaphoreGive(s_status_lock);
    }

    if (waiter != NULL) {
        xTaskNotifyGive(waiter);
    }

    for (;;) {
        voice_assistant_command_t command = {0};
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!voice_assistant_command_is_current(&command)) {
            continue;
        }

        switch (command.type) {
            case VOICE_ASSISTANT_COMMAND_BEGIN_SESSION:
                /*
                 * Phase 13-A deliberately stops at CONNECTING. READY requires
                 * real transport evidence from the Xiaozhi adapter in 13-B.
                 */
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_CONNECTING,
                    true,
                    ESP_OK);
                break;

            case VOICE_ASSISTANT_COMMAND_END_SESSION:
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_IDLE,
                    false,
                    ESP_OK);
                break;

            default:
                ESP_LOGE(TAG, "Unknown command=%d", (int)command.type);
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_ERROR,
                    false,
                    ESP_ERR_INVALID_ARG);
                break;
        }
    }
}

esp_err_t voice_assistant_init(void)
{
    if (s_status_lock != NULL) {
        return ESP_OK;
    }

    s_status_lock = xSemaphoreCreateMutex();
    if (s_status_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_command_queue = xQueueCreate(
        VOICE_ASSISTANT_COMMAND_QUEUE_LENGTH,
        sizeof(voice_assistant_command_t));
    if (s_command_queue == NULL) {
        vSemaphoreDelete(s_status_lock);
        s_status_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = VOICE_ASSISTANT_STATE_INITIALIZED;
    s_status.last_error = ESP_OK;
    s_command_pending = false;

    ESP_LOGI(TAG, "initialized");
    voice_assistant_publish_status();
    return ESP_OK;
}

esp_err_t voice_assistant_start(void)
{
    if ((s_status_lock == NULL) || (s_command_queue == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_task_handle != NULL) {
        xSemaphoreGive(s_status_lock);
        return ESP_OK;
    }

    if (s_status.state != VOICE_ASSISTANT_STATE_INITIALIZED) {
        xSemaphoreGive(s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_start_waiter = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(s_status_lock);

    const BaseType_t task_ret = xTaskCreate(
        voice_assistant_task,
        VOICE_ASSISTANT_TASK_NAME,
        VOICE_ASSISTANT_TASK_STACK_BYTES,
        NULL,
        VOICE_ASSISTANT_TASK_PRIORITY,
        &s_task_handle);

    if (task_ret != pdPASS) {
        if (voice_assistant_take_lock()) {
            s_start_waiter = NULL;
            xSemaphoreGive(s_status_lock);
        }
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(VOICE_ASSISTANT_START_TIMEOUT_MS)) == 0U) {
        ESP_LOGE(TAG, "start timeout waiting for IDLE");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t voice_assistant_begin_session(void)
{
    if ((s_status_lock == NULL) || (s_command_queue == NULL) ||
        (s_task_handle == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_BEGIN_SESSION,
        .generation = 0U,
    };

    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if ((s_status.state != VOICE_ASSISTANT_STATE_IDLE) ||
        s_status.session_active || s_command_pending) {
        xSemaphoreGive(s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status.session_generation == UINT32_MAX) {
        s_status.session_generation = 1U;
    } else {
        ++s_status.session_generation;
        if (s_status.session_generation == 0U) {
            s_status.session_generation = 1U;
        }
    }

    command.generation = s_status.session_generation;
    s_command_pending = true;
    xSemaphoreGive(s_status_lock);

    if (xQueueSend(s_command_queue, &command, 0U) != pdTRUE) {
        if (voice_assistant_take_lock()) {
            s_command_pending = false;
            xSemaphoreGive(s_status_lock);
        }
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "begin session queued generation=%u",
             (unsigned)command.generation);
    return ESP_OK;
}

esp_err_t voice_assistant_end_session(void)
{
    if ((s_status_lock == NULL) || (s_command_queue == NULL) ||
        (s_task_handle == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_END_SESSION,
        .generation = 0U,
    };

    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if ((s_status.state == VOICE_ASSISTANT_STATE_IDLE) ||
        (s_status.state == VOICE_ASSISTANT_STATE_INITIALIZED) ||
        (s_status.state == VOICE_ASSISTANT_STATE_UNINITIALIZED) ||
        !s_status.session_active || s_command_pending) {
        xSemaphoreGive(s_status_lock);
        return ESP_ERR_INVALID_STATE;
    }

    command.generation = s_status.session_generation;
    s_command_pending = true;
    xSemaphoreGive(s_status_lock);

    if (xQueueSend(s_command_queue, &command, 0U) != pdTRUE) {
        if (voice_assistant_take_lock()) {
            s_command_pending = false;
            xSemaphoreGive(s_status_lock);
        }
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "end session queued generation=%u",
             (unsigned)command.generation);
    return ESP_OK;
}

esp_err_t voice_assistant_register_status_callback(
    voice_assistant_status_callback_t callback,
    void *user_context)
{
    if (s_status_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    s_status_callback = callback;
    s_status_callback_context = user_context;
    xSemaphoreGive(s_status_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_get_status(voice_assistant_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_status_lock == NULL) {
        memset(status, 0, sizeof(*status));
        status->state = VOICE_ASSISTANT_STATE_UNINITIALIZED;
        return ESP_ERR_INVALID_STATE;
    }

    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    *status = s_status;
    xSemaphoreGive(s_status_lock);
    return ESP_OK;
}

const char *voice_assistant_state_to_string(voice_assistant_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_STATE_UNINITIALIZED:
            return "UNINITIALIZED";
        case VOICE_ASSISTANT_STATE_INITIALIZED:
            return "INITIALIZED";
        case VOICE_ASSISTANT_STATE_IDLE:
            return "IDLE";
        case VOICE_ASSISTANT_STATE_CONNECTING:
            return "CONNECTING";
        case VOICE_ASSISTANT_STATE_READY:
            return "READY";
        case VOICE_ASSISTANT_STATE_LISTENING:
            return "LISTENING";
        case VOICE_ASSISTANT_STATE_THINKING:
            return "THINKING";
        case VOICE_ASSISTANT_STATE_SPEAKING:
            return "SPEAKING";
        case VOICE_ASSISTANT_STATE_RECOVERING:
            return "RECOVERING";
        case VOICE_ASSISTANT_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
