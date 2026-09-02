#include "voice_assistant.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "xiaozhi_foundation.h"

#define VOICE_ASSISTANT_TASK_NAME            "voice_assistant"
#define VOICE_ASSISTANT_TASK_STACK_BYTES     4096U
#define VOICE_ASSISTANT_TASK_PRIORITY        4U
#define VOICE_ASSISTANT_COMMAND_QUEUE_LENGTH 8U
#define VOICE_ASSISTANT_LOCK_TIMEOUT_MS      100U
#define VOICE_ASSISTANT_START_TIMEOUT_MS     2000U

typedef enum {
    VOICE_ASSISTANT_COMMAND_BEGIN_SESSION = 0,
    VOICE_ASSISTANT_COMMAND_END_SESSION,
    VOICE_ASSISTANT_COMMAND_RECOVER,
    VOICE_ASSISTANT_COMMAND_FOUNDATION_STATUS,
    VOICE_ASSISTANT_COMMAND_AUDIO_STATUS,
} voice_assistant_command_type_t;

typedef struct {
    voice_assistant_command_type_t type;
    uint32_t generation;
    xiaozhi_foundation_session_status_t foundation_status;
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
    .audio = {
        .state = VOICE_ASSISTANT_AUDIO_UNAVAILABLE,
        .capture_active = false,
        .playback_active = false,
        .last_error = ESP_OK,
    },
};

static voice_assistant_status_callback_t s_status_callback = NULL;
static void *s_status_callback_context = NULL;
static bool s_command_pending = false;
static bool s_audio_status_pending = false;
static voice_assistant_audio_status_t s_pending_audio_status = {
    .state = VOICE_ASSISTANT_AUDIO_UNAVAILABLE,
    .capture_active = false,
    .playback_active = false,
    .last_error = ESP_OK,
};

static bool voice_assistant_take_lock(void)
{
    return (s_status_lock != NULL) &&
           (xSemaphoreTake(
                s_status_lock,
                pdMS_TO_TICKS(VOICE_ASSISTANT_LOCK_TIMEOUT_MS)) == pdTRUE);
}

static bool voice_assistant_audio_status_is_valid(
    const voice_assistant_audio_status_t *status)
{
    if (status == NULL) {
        return false;
    }
    if ((status->state < VOICE_ASSISTANT_AUDIO_UNAVAILABLE) ||
        (status->state > VOICE_ASSISTANT_AUDIO_ERROR)) {
        return false;
    }
    return !(status->capture_active && status->playback_active);
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
    xSemaphoreGive(s_status_lock);

    if ((previous_state != state) || (last_error != ESP_OK)) {
        ESP_LOGI(TAG,
                 "state %s -> %s generation=%u active=%s error=%s",
                 voice_assistant_state_to_string(previous_state),
                 voice_assistant_state_to_string(state),
                 (unsigned)generation,
                 session_active ? "yes" : "no",
                 esp_err_to_name(last_error));
    }
    voice_assistant_publish_status();
}

static void voice_assistant_set_audio_status(
    const voice_assistant_audio_status_t *audio_status)
{
    voice_assistant_audio_state_t previous = VOICE_ASSISTANT_AUDIO_UNAVAILABLE;

    if (!voice_assistant_audio_status_is_valid(audio_status)) {
        return;
    }
    if (!voice_assistant_take_lock()) {
        ESP_LOGE(TAG, "Audio status dropped: lock timeout");
        return;
    }
    previous = s_status.audio.state;
    s_status.audio = *audio_status;
    xSemaphoreGive(s_status_lock);

    if ((previous != audio_status->state) ||
        (audio_status->last_error != ESP_OK)) {
        ESP_LOGI(TAG,
                 "audio %s -> %s capture=%s playback=%s error=%s",
                 voice_assistant_audio_state_to_string(previous),
                 voice_assistant_audio_state_to_string(audio_status->state),
                 audio_status->capture_active ? "yes" : "no",
                 audio_status->playback_active ? "yes" : "no",
                 esp_err_to_name(audio_status->last_error));
    }
    voice_assistant_publish_status();
}

static bool voice_assistant_generation_is_current(uint32_t generation)
{
    bool current = false;
    if (!voice_assistant_take_lock()) {
        return false;
    }
    current = (generation != 0U) &&
              (generation == s_status.session_generation);
    xSemaphoreGive(s_status_lock);
    return current;
}

static voice_assistant_state_t voice_assistant_get_state_unlocked_copy(void)
{
    voice_assistant_state_t state = VOICE_ASSISTANT_STATE_UNINITIALIZED;
    if (!voice_assistant_take_lock()) {
        return state;
    }
    state = s_status.state;
    xSemaphoreGive(s_status_lock);
    return state;
}

static void voice_assistant_finish_public_command(void)
{
    if (!voice_assistant_take_lock()) {
        ESP_LOGE(TAG, "Unable to clear public-command gate: lock timeout");
        return;
    }
    s_command_pending = false;
    xSemaphoreGive(s_status_lock);
}

static void voice_assistant_foundation_status_callback(
    const xiaozhi_foundation_session_status_t *status,
    void *user_context)
{
    (void)user_context;
    if ((status == NULL) || (s_command_queue == NULL)) {
        return;
    }
    if ((status->state != XIAOZHI_FOUNDATION_SESSION_CONNECTING) &&
        (status->state != XIAOZHI_FOUNDATION_SESSION_READY) &&
        (status->state != XIAOZHI_FOUNDATION_SESSION_ERROR)) {
        return;
    }

    const voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_FOUNDATION_STATUS,
        .generation = status->client_generation,
        .foundation_status = *status,
    };
    if (xQueueSend(s_command_queue, &command, 0U) != pdTRUE) {
        ESP_LOGW(TAG,
                 "Dropped Xiaozhi status generation=%u state=%s: queue full",
                 (unsigned)status->client_generation,
                 xiaozhi_foundation_session_state_to_string(status->state));
    }
}

static void voice_assistant_handle_foundation_status(
    const voice_assistant_command_t *command)
{
    if (command == NULL) {
        return;
    }
    if (!voice_assistant_generation_is_current(command->generation)) {
        ESP_LOGW(TAG,
                 "Dropped stale Xiaozhi status generation=%u state=%s",
                 (unsigned)command->generation,
                 xiaozhi_foundation_session_state_to_string(
                     command->foundation_status.state));
        return;
    }

    const voice_assistant_state_t current =
        voice_assistant_get_state_unlocked_copy();

    switch (command->foundation_status.state) {
        case XIAOZHI_FOUNDATION_SESSION_CONNECTING:
            if ((current != VOICE_ASSISTANT_STATE_IDLE) &&
                (current != VOICE_ASSISTANT_STATE_INITIALIZED) &&
                (current != VOICE_ASSISTANT_STATE_UNINITIALIZED) &&
                (current != VOICE_ASSISTANT_STATE_RECOVERING)) {
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_CONNECTING,
                    command->foundation_status.active,
                    ESP_OK);
            }
            break;

        case XIAOZHI_FOUNDATION_SESSION_READY:
            if ((current == VOICE_ASSISTANT_STATE_CONNECTING) ||
                (current == VOICE_ASSISTANT_STATE_READY) ||
                ((current == VOICE_ASSISTANT_STATE_ERROR) &&
                 command->foundation_status.active)) {
                if (current != VOICE_ASSISTANT_STATE_READY) {
                    voice_assistant_set_status(
                        VOICE_ASSISTANT_STATE_READY,
                        true,
                        ESP_OK);
                }
            } else {
                ESP_LOGW(TAG,
                         "Ignored READY in voice state=%s generation=%u",
                         voice_assistant_state_to_string(current),
                         (unsigned)command->generation);
            }
            break;

        case XIAOZHI_FOUNDATION_SESSION_ERROR:
            /* An intentional stop may deliver DISCONNECTED after the end
             * command has already returned the orchestrator to IDLE. Do not
             * regress a completed stop into ERROR. */
            if ((current == VOICE_ASSISTANT_STATE_IDLE) ||
                (current == VOICE_ASSISTANT_STATE_RECOVERING)) {
                ESP_LOGI(TAG,
                         "Ignored late Xiaozhi ERROR in state=%s generation=%u",
                         voice_assistant_state_to_string(current),
                         (unsigned)command->generation);
                break;
            }
            voice_assistant_set_status(
                VOICE_ASSISTANT_STATE_ERROR,
                command->foundation_status.active,
                (command->foundation_status.last_error == ESP_OK) ?
                    ESP_FAIL : command->foundation_status.last_error);
            break;

        default:
            break;
    }
}

static void voice_assistant_handle_audio_marker(void)
{
    voice_assistant_audio_status_t latest = {0};
    if (!voice_assistant_take_lock()) {
        ESP_LOGE(TAG, "Audio marker dropped: lock timeout");
        return;
    }
    latest = s_pending_audio_status;
    s_audio_status_pending = false;
    xSemaphoreGive(s_status_lock);
    voice_assistant_set_audio_status(&latest);
}

static void voice_assistant_task(void *argument)
{
    (void)argument;
    voice_assistant_set_status(VOICE_ASSISTANT_STATE_IDLE, false, ESP_OK);

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

        switch (command.type) {
            case VOICE_ASSISTANT_COMMAND_BEGIN_SESSION: {
                if (!voice_assistant_generation_is_current(command.generation)) {
                    ESP_LOGW(TAG, "Dropped stale begin command generation=%u",
                             (unsigned)command.generation);
                    voice_assistant_finish_public_command();
                    break;
                }
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_CONNECTING, true, ESP_OK);
                const esp_err_t ret =
                    xiaozhi_foundation_session_start(command.generation);
                if (ret == ESP_OK) {
                    xiaozhi_foundation_session_status_t foundation = {0};
                    const esp_err_t status_ret =
                        xiaozhi_foundation_session_get_status(&foundation);
                    voice_assistant_set_status(
                        ((status_ret == ESP_OK) &&
                         (foundation.state ==
                          XIAOZHI_FOUNDATION_SESSION_CONNECTING)) ?
                            VOICE_ASSISTANT_STATE_CONNECTING :
                            VOICE_ASSISTANT_STATE_READY,
                        (status_ret == ESP_OK) ? foundation.active : true,
                        ESP_OK);
                } else {
                    voice_assistant_set_status(
                        VOICE_ASSISTANT_STATE_ERROR, false, ret);
                }
                voice_assistant_finish_public_command();
                break;
            }

            case VOICE_ASSISTANT_COMMAND_END_SESSION: {
                if (!voice_assistant_generation_is_current(command.generation)) {
                    ESP_LOGW(TAG, "Dropped stale end command generation=%u",
                             (unsigned)command.generation);
                    voice_assistant_finish_public_command();
                    break;
                }
                const esp_err_t ret = xiaozhi_foundation_session_stop();
                if (ret == ESP_OK) {
                    voice_assistant_set_status(
                        VOICE_ASSISTANT_STATE_IDLE, false, ESP_OK);
                } else {
                    voice_assistant_set_status(
                        VOICE_ASSISTANT_STATE_ERROR, false, ret);
                }
                voice_assistant_finish_public_command();
                break;
            }

            case VOICE_ASSISTANT_COMMAND_RECOVER: {
                if (!voice_assistant_generation_is_current(command.generation)) {
                    ESP_LOGW(TAG, "Dropped stale recover command generation=%u",
                             (unsigned)command.generation);
                    voice_assistant_finish_public_command();
                    break;
                }
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_RECOVERING, true, s_status.last_error);

                xiaozhi_foundation_session_status_t foundation = {0};
                esp_err_t ret = xiaozhi_foundation_session_get_status(&foundation);
                if ((ret == ESP_OK) && foundation.active) {
                    ret = xiaozhi_foundation_session_stop();
                } else if (ret == ESP_OK) {
                    ret = ESP_OK;
                }

                if (ret == ESP_OK) {
                    voice_assistant_set_status(
                        VOICE_ASSISTANT_STATE_IDLE, false, ESP_OK);
                } else {
                    voice_assistant_set_status(
                        VOICE_ASSISTANT_STATE_ERROR, false, ret);
                }
                voice_assistant_finish_public_command();
                break;
            }

            case VOICE_ASSISTANT_COMMAND_FOUNDATION_STATUS:
                voice_assistant_handle_foundation_status(&command);
                break;

            case VOICE_ASSISTANT_COMMAND_AUDIO_STATUS:
                voice_assistant_handle_audio_marker();
                break;

            default:
                ESP_LOGE(TAG, "Unknown command=%d", (int)command.type);
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_ERROR, false, ESP_ERR_INVALID_ARG);
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
    s_status.audio.state = VOICE_ASSISTANT_AUDIO_UNAVAILABLE;
    s_status.audio.last_error = ESP_OK;
    s_command_pending = false;
    s_audio_status_pending = false;
    s_pending_audio_status.state = VOICE_ASSISTANT_AUDIO_UNAVAILABLE;
    s_pending_audio_status.last_error = ESP_OK;

    const esp_err_t observer_ret =
        xiaozhi_foundation_session_register_status_callback(
            voice_assistant_foundation_status_callback, NULL);
    if (observer_ret != ESP_OK) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        vSemaphoreDelete(s_status_lock);
        s_status_lock = NULL;
        memset(&s_status, 0, sizeof(s_status));
        s_status.state = VOICE_ASSISTANT_STATE_UNINITIALIZED;
        s_status.audio.state = VOICE_ASSISTANT_AUDIO_UNAVAILABLE;
        return observer_ret;
    }

    ESP_LOGI(TAG, "initialized with Xiaozhi session observer");
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

esp_err_t voice_assistant_recover(void)
{
    if ((s_status_lock == NULL) || (s_command_queue == NULL) ||
        (s_task_handle == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_RECOVER,
        .generation = 0U,
    };
    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if ((s_status.state != VOICE_ASSISTANT_STATE_ERROR) || s_command_pending ||
        (s_status.session_generation == 0U)) {
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
    ESP_LOGI(TAG, "recovery queued generation=%u",
             (unsigned)command.generation);
    return ESP_OK;
}

esp_err_t voice_assistant_notify_audio_status(
    const voice_assistant_audio_status_t *status)
{
    if (!voice_assistant_audio_status_is_valid(status)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_command_queue == NULL) || (s_task_handle == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    bool enqueue_marker = false;
    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    s_pending_audio_status = *status;
    if (!s_audio_status_pending) {
        s_audio_status_pending = true;
        enqueue_marker = true;
    }
    xSemaphoreGive(s_status_lock);

    if (!enqueue_marker) {
        return ESP_OK;
    }

    const voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_AUDIO_STATUS,
    };
    if (xQueueSend(s_command_queue, &command, 0U) != pdTRUE) {
        if (voice_assistant_take_lock()) {
            s_audio_status_pending = false;
            xSemaphoreGive(s_status_lock);
        }
        return ESP_ERR_TIMEOUT;
    }
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
        status->audio.state = VOICE_ASSISTANT_AUDIO_UNAVAILABLE;
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
        case VOICE_ASSISTANT_STATE_UNINITIALIZED: return "UNINITIALIZED";
        case VOICE_ASSISTANT_STATE_INITIALIZED: return "INITIALIZED";
        case VOICE_ASSISTANT_STATE_IDLE: return "IDLE";
        case VOICE_ASSISTANT_STATE_CONNECTING: return "CONNECTING";
        case VOICE_ASSISTANT_STATE_READY: return "READY";
        case VOICE_ASSISTANT_STATE_LISTENING: return "LISTENING";
        case VOICE_ASSISTANT_STATE_THINKING: return "THINKING";
        case VOICE_ASSISTANT_STATE_SPEAKING: return "SPEAKING";
        case VOICE_ASSISTANT_STATE_RECOVERING: return "RECOVERING";
        case VOICE_ASSISTANT_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char *voice_assistant_audio_state_to_string(
    voice_assistant_audio_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_AUDIO_UNAVAILABLE: return "UNAVAILABLE";
        case VOICE_ASSISTANT_AUDIO_INITIALIZED: return "INITIALIZED";
        case VOICE_ASSISTANT_AUDIO_IDLE: return "IDLE";
        case VOICE_ASSISTANT_AUDIO_RECORDING: return "RECORDING";
        case VOICE_ASSISTANT_AUDIO_PROCESSING: return "PROCESSING";
        case VOICE_ASSISTANT_AUDIO_PLAYBACK: return "PLAYBACK";
        case VOICE_ASSISTANT_AUDIO_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
