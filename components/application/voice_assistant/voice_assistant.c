#include "voice_assistant.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_log.h"

#include "app_network_coordinator.h"
#include "xiaozhi_foundation.h"

#define VOICE_ASSISTANT_TASK_NAME            "voice_assistant"
/* HIL observed the session/status worker exhausting 4 KiB while WebSocket,
 * SNTP, and playback-arbitration activity overlapped. Keep 8 KiB internal
 * stack so this coordinator cannot reboot the board mid-response. */
#define VOICE_ASSISTANT_TASK_STACK_BYTES     8192U
#define VOICE_ASSISTANT_TASK_PRIORITY        4U
#define VOICE_ASSISTANT_COMMAND_QUEUE_LENGTH 8U
#define VOICE_ASSISTANT_LOCK_TIMEOUT_MS      100U
#define VOICE_ASSISTANT_START_TIMEOUT_MS     2000U
#define VOICE_ASSISTANT_RECOVERY_POLL_MS     250U
#define VOICE_ASSISTANT_RETRY_INITIAL_MS     5000U
#define VOICE_ASSISTANT_RETRY_MAX_MS         60000U

typedef enum {
    VOICE_ASSISTANT_COMMAND_BEGIN_SESSION = 0,
    VOICE_ASSISTANT_COMMAND_END_SESSION,
    VOICE_ASSISTANT_COMMAND_RECOVER,
    VOICE_ASSISTANT_COMMAND_AUTO_RECOVER,
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
/* Owned only by the voice orchestration task. The retry policy never owns
 * Wi-Fi: it reads the coordinator's ONLINE snapshot before it recreates a
 * failed Xiaozhi service session. */
static bool s_auto_recovery_scheduled = false;
static bool s_auto_recovery_waiting_for_network = false;
static TickType_t s_auto_recovery_due_at = 0U;
static uint32_t s_auto_recovery_attempt = 0U;
/* Foundation and audio producers may run while the bounded command queue is
 * full. Keep one latest copied snapshot for each source; a queue marker only
 * wakes the coordinator, while the task also drains these snapshots after
 * every command. */
static portMUX_TYPE s_pending_status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_foundation_status_pending = false;
static xiaozhi_foundation_session_status_t s_pending_foundation_status = {
    .state = XIAOZHI_FOUNDATION_SESSION_STOPPED,
    .client_generation = 0U,
    .active = false,
    .last_error = ESP_OK,
};
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

static void voice_assistant_schedule_auto_recovery(esp_err_t error);
static void voice_assistant_cancel_auto_recovery(void);
static void voice_assistant_poll_auto_recovery(void);
static esp_err_t voice_assistant_queue_auto_recovery(void);

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
        APP_LOGW(TAG, STATUS_PUBLISH_SKIPPED_LOCK_D87749BC, "Status publish skipped: lock timeout");
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
        APP_LOGE(TAG, STATE_TRANSITION_DROPPED_LOC_9956CA73, "State transition dropped: lock timeout");
        return;
    }
    previous_state = s_status.state;
    s_status.state = state;
    s_status.session_active = session_active;
    s_status.last_error = last_error;
    generation = s_status.session_generation;
    xSemaphoreGive(s_status_lock);

    if ((previous_state != state) || (last_error != ESP_OK)) {
        APP_LOGI(TAG, STATE_S_S_GENERATION_U_EC75E51B,
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
        APP_LOGE(TAG, AUDIO_STATUS_DROPPED_LOCK_TI_0FCDDB94, "Audio status dropped: lock timeout");
        return;
    }
    previous = s_status.audio.state;
    s_status.audio = *audio_status;
    xSemaphoreGive(s_status_lock);

    if ((previous != audio_status->state) ||
        (audio_status->last_error != ESP_OK)) {
        APP_LOGI(TAG, AUDIO_S_S_CAPTURE_S_AC4F9F5B,
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
        APP_LOGE(TAG, UNABLE_TO_CLEAR_PUBLIC_COMMA_BF3173F0, "Unable to clear public-command gate: lock timeout");
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

    bool enqueue_marker = false;
    portENTER_CRITICAL(&s_pending_status_lock);
    s_pending_foundation_status = *status;
    if (!s_foundation_status_pending) {
        s_foundation_status_pending = true;
        enqueue_marker = true;
    }
    portEXIT_CRITICAL(&s_pending_status_lock);

    if (!enqueue_marker) {
        return;
    }

    const voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_FOUNDATION_STATUS,
    };
    if (xQueueSend(s_command_queue, &command, 0U) != pdTRUE) {
        APP_LOGW(TAG, DEFERRED_XIAOZHI_STATUS_GENE_80A36D00,
                 "Deferred Xiaozhi status generation=%u state=%s: queue full",
                 (unsigned)status->client_generation,
                 xiaozhi_foundation_session_state_to_string(status->state));
    }
}

static void voice_assistant_handle_foundation_status(void)
{
    xiaozhi_foundation_session_status_t foundation_status = {0};
    bool pending = false;

    portENTER_CRITICAL(&s_pending_status_lock);
    pending = s_foundation_status_pending;
    if (pending) {
        foundation_status = s_pending_foundation_status;
        s_foundation_status_pending = false;
    }
    portEXIT_CRITICAL(&s_pending_status_lock);

    if (!pending) {
        return;
    }
    if (!voice_assistant_generation_is_current(
            foundation_status.client_generation)) {
        APP_LOGW(TAG, DROPPED_STALE_XIAOZHI_STATUS_3C381E79,
                 "Dropped stale Xiaozhi status generation=%u state=%s",
                 (unsigned)foundation_status.client_generation,
                 xiaozhi_foundation_session_state_to_string(
                     foundation_status.state));
        return;
    }

    const voice_assistant_state_t current =
        voice_assistant_get_state_unlocked_copy();

    switch (foundation_status.state) {
        case XIAOZHI_FOUNDATION_SESSION_CONNECTING:
            /* session_start() publishes its initial inactive CONNECTING
             * snapshot before the actual WebSocket CONNECTED callback. The
             * asynchronous voice queue may consume that older snapshot after
             * READY, so it must not briefly regress the production state or
             * create a PTT arming race at boot. Transport-loss reconnects
             * retain active=true and remain observable below. */
            if ((current == VOICE_ASSISTANT_STATE_READY) &&
                !foundation_status.active) {
                APP_LOGD(TAG, IGNORED_STALE_INACTIVE_CONNE_45346E2C,
                         "Ignored stale inactive CONNECTING generation=%u",
                         (unsigned)foundation_status.client_generation);
                break;
            }
            if ((current != VOICE_ASSISTANT_STATE_IDLE) &&
                (current != VOICE_ASSISTANT_STATE_INITIALIZED) &&
                (current != VOICE_ASSISTANT_STATE_UNINITIALIZED) &&
                (current != VOICE_ASSISTANT_STATE_RECOVERING)) {
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_CONNECTING,
                    foundation_status.active,
                    ESP_OK);
            }
            break;

        case XIAOZHI_FOUNDATION_SESSION_READY:
            voice_assistant_cancel_auto_recovery();
            if ((current == VOICE_ASSISTANT_STATE_CONNECTING) ||
                (current == VOICE_ASSISTANT_STATE_READY) ||
                ((current == VOICE_ASSISTANT_STATE_ERROR) &&
                 foundation_status.active)) {
                if (current != VOICE_ASSISTANT_STATE_READY) {
                    voice_assistant_set_status(
                        VOICE_ASSISTANT_STATE_READY,
                        true,
                        ESP_OK);
                }
            } else {
                APP_LOGW(TAG, IGNORED_READY_IN_VOICE_STATE_860F729E,
                         "Ignored READY in voice state=%s generation=%u",
                         voice_assistant_state_to_string(current),
                         (unsigned)foundation_status.client_generation);
            }
            break;

        case XIAOZHI_FOUNDATION_SESSION_ERROR:
            /* An intentional stop may deliver DISCONNECTED after the end
             * command has already returned the orchestrator to IDLE. Do not
             * regress a completed stop into ERROR. */
            if ((current == VOICE_ASSISTANT_STATE_IDLE) ||
                (current == VOICE_ASSISTANT_STATE_RECOVERING)) {
                APP_LOGI(TAG, IGNORED_LATE_XIAOZHI_ERROR_I_50936368,
                          "Ignored late Xiaozhi ERROR in state=%s generation=%u",
                          voice_assistant_state_to_string(current),
                          (unsigned)foundation_status.client_generation);
                break;
            }
            voice_assistant_set_status(
                VOICE_ASSISTANT_STATE_ERROR,
                foundation_status.active,
                (foundation_status.last_error == ESP_OK) ?
                    ESP_FAIL : foundation_status.last_error);
            /* A retained active session belongs to the provider's reconnect
             * loop. A terminal inactive session needs product-level recovery. */
            if (!foundation_status.active) {
                voice_assistant_schedule_auto_recovery(
                    foundation_status.last_error);
            }
            break;

        default:
            break;
    }
}

static void voice_assistant_handle_audio_marker(void)
{
    voice_assistant_audio_status_t latest = {0};
    bool pending = false;

    portENTER_CRITICAL(&s_pending_status_lock);
    pending = s_audio_status_pending;
    if (pending) {
        latest = s_pending_audio_status;
        s_audio_status_pending = false;
    }
    portEXIT_CRITICAL(&s_pending_status_lock);

    if (!pending) {
        return;
    }
    voice_assistant_set_audio_status(&latest);
}

static uint32_t voice_assistant_auto_recovery_delay_ms(uint32_t attempt)
{
    uint32_t delay_ms = VOICE_ASSISTANT_RETRY_INITIAL_MS;
    while ((attempt > 1U) && (delay_ms < VOICE_ASSISTANT_RETRY_MAX_MS)) {
        const uint32_t remaining = VOICE_ASSISTANT_RETRY_MAX_MS - delay_ms;
        delay_ms += (delay_ms > remaining) ? remaining : delay_ms;
        --attempt;
    }
    return delay_ms;
}

static void voice_assistant_schedule_auto_recovery(esp_err_t error)
{
    if (s_auto_recovery_scheduled) {
        return;
    }
    if (s_auto_recovery_attempt < UINT32_MAX) {
        ++s_auto_recovery_attempt;
    }
    const uint32_t delay_ms =
        voice_assistant_auto_recovery_delay_ms(s_auto_recovery_attempt);
    s_auto_recovery_due_at = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    s_auto_recovery_scheduled = true;
    s_auto_recovery_waiting_for_network = false;
    APP_LOGW(TAG, AUTO_RECOVERY_SCHEDULED_ATTEM_76EF3ED9,
             "auto recovery scheduled attempt=%u delay_ms=%u error=%s",
             (unsigned)s_auto_recovery_attempt,
             (unsigned)delay_ms,
             esp_err_to_name((error == ESP_OK) ? ESP_FAIL : error));
}

static void voice_assistant_cancel_auto_recovery(void)
{
    if (s_auto_recovery_scheduled || (s_auto_recovery_attempt != 0U)) {
        APP_LOGI(TAG, AUTO_RECOVERY_RESET_8A64E31B,
                 "auto recovery reset after Xiaozhi READY attempts=%u",
                 (unsigned)s_auto_recovery_attempt);
    }
    s_auto_recovery_scheduled = false;
    s_auto_recovery_waiting_for_network = false;
    s_auto_recovery_due_at = 0U;
    s_auto_recovery_attempt = 0U;
}

static esp_err_t voice_assistant_queue_auto_recovery(void)
{
    voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_AUTO_RECOVER,
        .generation = 0U,
    };
    if (!voice_assistant_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if ((s_status.state != VOICE_ASSISTANT_STATE_ERROR) ||
        s_command_pending || (s_status.session_generation == 0U)) {
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
    APP_LOGI(TAG, AUTO_RECOVERY_QUEUED_GENERATI_16D86980,
             "auto recovery queued generation=%u attempt=%u",
             (unsigned)command.generation,
             (unsigned)s_auto_recovery_attempt);
    return ESP_OK;
}

static void voice_assistant_poll_auto_recovery(void)
{
    if (!s_auto_recovery_scheduled) {
        return;
    }
    if (voice_assistant_get_state_unlocked_copy() !=
        VOICE_ASSISTANT_STATE_ERROR) {
        s_auto_recovery_scheduled = false;
        s_auto_recovery_waiting_for_network = false;
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - s_auto_recovery_due_at) < 0) {
        return;
    }

    app_network_coordinator_state_t network_state =
        APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED;
    const esp_err_t network_ret =
        app_network_coordinator_get_state(&network_state);
    if ((network_ret != ESP_OK) ||
        (network_state != APP_NETWORK_COORDINATOR_STATE_ONLINE)) {
        if (!s_auto_recovery_waiting_for_network) {
            APP_LOGI(TAG, AUTO_RECOVERY_WAITING_NETWO_60C1BB73,
                     "auto recovery waiting for network state=%s status=%s",
                     app_network_coordinator_state_to_string(network_state),
                     esp_err_to_name(network_ret));
            s_auto_recovery_waiting_for_network = true;
        }
        s_auto_recovery_due_at =
            now + pdMS_TO_TICKS(VOICE_ASSISTANT_RECOVERY_POLL_MS);
        return;
    }

    s_auto_recovery_scheduled = false;
    s_auto_recovery_waiting_for_network = false;
    const esp_err_t queue_ret = voice_assistant_queue_auto_recovery();
    if (queue_ret != ESP_OK) {
        APP_LOGW(TAG, AUTO_RECOVERY_QUEUE_FAILED_A_0E42B45E,
                 "auto recovery queue failed attempt=%u: %s",
                 (unsigned)s_auto_recovery_attempt,
                 esp_err_to_name(queue_ret));
        voice_assistant_schedule_auto_recovery(queue_ret);
    }
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
        if (xQueueReceive(s_command_queue, &command,
                          pdMS_TO_TICKS(VOICE_ASSISTANT_RECOVERY_POLL_MS)) == pdTRUE) {
            switch (command.type) {
            case VOICE_ASSISTANT_COMMAND_BEGIN_SESSION: {
                if (!voice_assistant_generation_is_current(command.generation)) {
                    APP_LOGW(TAG, DROPPED_STALE_BEGIN_COMMAND_E0C8D9F9, "Dropped stale begin command generation=%u",
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
                    voice_assistant_schedule_auto_recovery(ret);
                }
                voice_assistant_finish_public_command();
                break;
            }

            case VOICE_ASSISTANT_COMMAND_END_SESSION: {
                if (!voice_assistant_generation_is_current(command.generation)) {
                    APP_LOGW(TAG, DROPPED_STALE_END_COMMAND_GE_64453AAD, "Dropped stale end command generation=%u",
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

            case VOICE_ASSISTANT_COMMAND_RECOVER:
            case VOICE_ASSISTANT_COMMAND_AUTO_RECOVER: {
                const bool automatic =
                    command.type == VOICE_ASSISTANT_COMMAND_AUTO_RECOVER;
                if (!voice_assistant_generation_is_current(command.generation)) {
                    APP_LOGW(TAG, DROPPED_STALE_RECOVER_COMMAN_A69B74E1, "Dropped stale recover command generation=%u",
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
                if (automatic && (ret == ESP_OK)) {
                    const esp_err_t begin_ret = voice_assistant_begin_session();
                    if (begin_ret != ESP_OK) {
                        voice_assistant_set_status(
                            VOICE_ASSISTANT_STATE_ERROR, false, begin_ret);
                        voice_assistant_schedule_auto_recovery(begin_ret);
                    }
                } else if (automatic) {
                    voice_assistant_schedule_auto_recovery(ret);
                }
                break;
            }

            case VOICE_ASSISTANT_COMMAND_FOUNDATION_STATUS:
                voice_assistant_handle_foundation_status();
                break;

            case VOICE_ASSISTANT_COMMAND_AUDIO_STATUS:
                voice_assistant_handle_audio_marker();
                break;

            default:
                APP_LOGE(TAG, UNKNOWN_COMMAND_D_60C4DA83, "Unknown command=%d", (int)command.type);
                voice_assistant_set_status(
                    VOICE_ASSISTANT_STATE_ERROR, false, ESP_ERR_INVALID_ARG);
                break;
            }
        }

        /* A full queue can prevent a source marker from being inserted. The
         * command just consumed guarantees forward progress for the latest
         * copied source snapshots without running callback work here. */
        voice_assistant_handle_foundation_status();
        voice_assistant_handle_audio_marker();
        voice_assistant_poll_auto_recovery();
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
    s_auto_recovery_scheduled = false;
    s_auto_recovery_waiting_for_network = false;
    s_auto_recovery_due_at = 0U;
    s_auto_recovery_attempt = 0U;
    portENTER_CRITICAL(&s_pending_status_lock);
    s_foundation_status_pending = false;
    s_pending_foundation_status = (xiaozhi_foundation_session_status_t) {
        .state = XIAOZHI_FOUNDATION_SESSION_STOPPED,
        .client_generation = 0U,
        .active = false,
        .last_error = ESP_OK,
    };
    s_audio_status_pending = false;
    s_pending_audio_status = (voice_assistant_audio_status_t) {
        .state = VOICE_ASSISTANT_AUDIO_UNAVAILABLE,
        .capture_active = false,
        .playback_active = false,
        .last_error = ESP_OK,
    };
    portEXIT_CRITICAL(&s_pending_status_lock);

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

    APP_LOGI(TAG, INITIALIZED_WITH_XIAOZHI_SES_ED17880B, "initialized with Xiaozhi session observer");
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
        APP_LOGE(TAG, START_TIMEOUT_WAITING_FOR_ID_F5455721, "start timeout waiting for IDLE");
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
    APP_LOGI(TAG, BEGIN_SESSION_QUEUED_GENERAT_D5F4C4B2, "begin session queued generation=%u",
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
    APP_LOGI(TAG, END_SESSION_QUEUED_GENERATIO_84CA4638, "end session queued generation=%u",
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
    APP_LOGI(TAG, RECOVERY_QUEUED_GENERATION_U_FA475BA5, "recovery queued generation=%u",
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
    portENTER_CRITICAL(&s_pending_status_lock);
    s_pending_audio_status = *status;
    if (!s_audio_status_pending) {
        s_audio_status_pending = true;
        enqueue_marker = true;
    }
    portEXIT_CRITICAL(&s_pending_status_lock);

    if (!enqueue_marker) {
        return ESP_OK;
    }

    const voice_assistant_command_t command = {
        .type = VOICE_ASSISTANT_COMMAND_AUDIO_STATUS,
    };
    if (xQueueSend(s_command_queue, &command, 0U) != pdTRUE) {
        APP_LOGW(TAG, DEFERRED_AUDIO_STATUS_COMMAN_74A2683A, "Deferred audio status: command queue full");
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
