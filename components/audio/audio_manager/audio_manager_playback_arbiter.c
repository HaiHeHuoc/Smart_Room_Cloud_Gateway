#include "audio_manager_playback_arbiter.h"

#include <string.h>

#include "audio_manager.h"
#include "audio_manager_pcm_stream.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PLAYBACK_ARBITER_TASK_NAME       "audio_pb_arb"
#define PLAYBACK_ARBITER_TASK_STACK      4096U
#define PLAYBACK_ARBITER_TASK_PRIORITY   5U
#define PLAYBACK_ARBITER_POLL_MS         50U
#define PLAYBACK_ARBITER_LOCK_MS         100U
#define PLAYBACK_ARBITER_START_MS        2000U
#define PLAYBACK_ARBITER_TERMINAL_HISTORY_LENGTH 4U

typedef enum {
    PLAYBACK_SOURCE_WAV = 0,
    PLAYBACK_SOURCE_PCM16_STREAM,
} playback_source_kind_t;

typedef struct {
    audio_manager_request_t request;
    playback_source_kind_t source;
    char path[AUDIO_MANAGER_WAV_PATH_MAX_BYTES];
    bool start_submitted;
    bool stream_finish_requested;
    bool cancel_requested;
    bool preempt_requested;
    bool failure_requested;
    esp_err_t requested_failure;
    audio_manager_playback_request_status_t stream;
} playback_slot_t;

static const char *const TAG = "AUDIO_PB_ARB";

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static TaskHandle_t s_start_waiter = NULL;
static playback_slot_t s_current = {0};
static playback_slot_t s_pending = {0};
static bool s_current_valid = false;
static bool s_pending_valid = false;
static audio_manager_playback_request_status_t
    s_terminal_history[PLAYBACK_ARBITER_TERMINAL_HISTORY_LENGTH] = {0};
static size_t s_terminal_next = 0U;
static audio_manager_playback_arbiter_status_t s_status = {0};

static bool take_lock(void)
{
    return (s_lock != NULL) &&
           (xSemaphoreTake(s_lock, pdMS_TO_TICKS(PLAYBACK_ARBITER_LOCK_MS)) == pdTRUE);
}

static void clear_slot(playback_slot_t *slot)
{
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

static audio_manager_playback_arbiter_state_t visible_state_locked(void)
{
    if (!s_current_valid) {
        return AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE;
    }
    if (s_current.cancel_requested || s_current.preempt_requested ||
        s_current.failure_requested) {
        return AUDIO_MANAGER_PLAYBACK_ARBITER_PREEMPTING;
    }
    switch (s_current.stream.state) {
        case AUDIO_MANAGER_PLAYBACK_REQUEST_STARTING:
            return AUDIO_MANAGER_PLAYBACK_ARBITER_STARTING;
        case AUDIO_MANAGER_PLAYBACK_REQUEST_ACTIVE:
        case AUDIO_MANAGER_PLAYBACK_REQUEST_DRAINING:
            return AUDIO_MANAGER_PLAYBACK_ARBITER_ACTIVE;
        default:
            return AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE;
    }
}

static void sync_status_locked(audio_manager_playback_arbiter_state_t state,
                               esp_err_t last_error)
{
    s_status.state = state;
    s_status.current_valid = s_current_valid;
    s_status.pending_valid = s_pending_valid;
    s_status.current = s_current_valid ? s_current.request : (audio_manager_request_t){0};
    s_status.pending = s_pending_valid ? s_pending.request : (audio_manager_request_t){0};
    s_status.last_error = last_error;
}

static void update_stream_metrics_locked(playback_slot_t *slot)
{
    if ((slot == NULL) || (slot->source != PLAYBACK_SOURCE_PCM16_STREAM)) {
        return;
    }

    audio_manager_pcm_stream_status_t manager_stream = {0};
    if (audio_manager_pcm_stream_get_status(
            slot->request.request_id,
            &manager_stream) != ESP_OK) {
        return;
    }

    slot->stream.pcm_samples_accepted = manager_stream.accepted_samples;
    slot->stream.pcm_samples_played = manager_stream.played_samples;
    slot->stream.ingress_queue_high_water = manager_stream.high_water_samples;
    slot->stream.ingress_full_count = manager_stream.full_count;
    slot->stream.starvation_count = manager_stream.starvation_count;
}

static void store_terminal_locked(const playback_slot_t *slot,
                                  audio_manager_playback_request_state_t state,
                                  esp_err_t result)
{
    if (slot == NULL) {
        return;
    }

    audio_manager_playback_request_status_t terminal = slot->stream;
    terminal.request_id = slot->request.request_id;
    terminal.state = state;
    terminal.result = result;
    s_terminal_history[s_terminal_next] = terminal;
    s_terminal_next = (s_terminal_next + 1U) %
                      PLAYBACK_ARBITER_TERMINAL_HISTORY_LENGTH;

    if (state == AUDIO_MANAGER_PLAYBACK_REQUEST_PREEMPTED) {
        ++s_status.preemption_count;
    } else if (state == AUDIO_MANAGER_PLAYBACK_REQUEST_COMPLETED) {
        ++s_status.completed_count;
    } else if (state == AUDIO_MANAGER_PLAYBACK_REQUEST_FAILED) {
        ++s_status.failed_count;
    }
}

static void promote_pending_locked(void)
{
    if (s_pending_valid) {
        s_current = s_pending;
        s_current_valid = true;
        clear_slot(&s_pending);
        s_pending_valid = false;
    }
}

static void finish_current_locked(esp_err_t manager_result)
{
    if (!s_current_valid) {
        return;
    }

    update_stream_metrics_locked(&s_current);
    audio_manager_playback_request_state_t terminal_state;
    esp_err_t terminal_result = manager_result;
    if (s_current.failure_requested) {
        terminal_state = AUDIO_MANAGER_PLAYBACK_REQUEST_FAILED;
        terminal_result = (s_current.requested_failure == ESP_OK)
            ? ESP_FAIL
            : s_current.requested_failure;
    } else if (s_current.preempt_requested) {
        terminal_state = AUDIO_MANAGER_PLAYBACK_REQUEST_PREEMPTED;
        terminal_result = ESP_OK;
    } else if (s_current.cancel_requested) {
        terminal_state = AUDIO_MANAGER_PLAYBACK_REQUEST_CANCELLED;
        terminal_result = ESP_OK;
    } else if ((s_current.source == PLAYBACK_SOURCE_PCM16_STREAM) &&
               !s_current.stream_finish_requested) {
        terminal_state = AUDIO_MANAGER_PLAYBACK_REQUEST_FAILED;
        terminal_result = ESP_ERR_INVALID_STATE;
    } else if (manager_result == ESP_OK) {
        terminal_state = AUDIO_MANAGER_PLAYBACK_REQUEST_COMPLETED;
        terminal_result = ESP_OK;
    } else {
        terminal_state = AUDIO_MANAGER_PLAYBACK_REQUEST_FAILED;
    }

    store_terminal_locked(&s_current, terminal_state, terminal_result);
    clear_slot(&s_current);
    s_current_valid = false;
    promote_pending_locked();
    sync_status_locked(visible_state_locked(), terminal_result);
}

static bool slot_has_request_id(const playback_slot_t *slot,
                                bool valid,
                                uint32_t request_id)
{
    return valid && (slot != NULL) &&
           (slot->request.request_id == request_id);
}

static bool request_is_stream_locked(uint32_t request_id,
                                     playback_slot_t **slot_out)
{
    playback_slot_t *slot = NULL;
    if (slot_has_request_id(&s_current, s_current_valid, request_id)) {
        slot = &s_current;
    } else if (slot_has_request_id(&s_pending, s_pending_valid, request_id)) {
        slot = &s_pending;
    }
    if ((slot == NULL) || (slot->source != PLAYBACK_SOURCE_PCM16_STREAM)) {
        return false;
    }
    if (slot_out != NULL) {
        *slot_out = slot;
    }
    return true;
}

static esp_err_t submit_slot_locked(const playback_slot_t *incoming)
{
    if (incoming == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_current_valid &&
         (s_current.request.request_id == incoming->request.request_id)) ||
        (s_pending_valid &&
         (s_pending.request.request_id == incoming->request.request_id))) {
        ++s_status.rejected_count;
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_current_valid) {
        s_current = *incoming;
        s_current_valid = true;
        ++s_status.accepted_count;
        sync_status_locked(visible_state_locked(), ESP_OK);
        return ESP_OK;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    switch (incoming->request.busy_policy) {
        case AUDIO_MANAGER_BUSY_REJECT:
            break;

        case AUDIO_MANAGER_BUSY_QUEUE:
            if (!s_pending_valid) {
                s_pending = *incoming;
                s_pending_valid = true;
                ++s_status.accepted_count;
                ++s_status.queued_count;
                result = ESP_OK;
            }
            break;

        case AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY:
            if (s_current.request.interruptible &&
                (incoming->request.priority > s_current.request.priority) &&
                !s_pending_valid) {
                s_pending = *incoming;
                s_pending_valid = true;
                s_current.preempt_requested = true;
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
    sync_status_locked(visible_state_locked(), s_status.last_error);
    return result;
}

static bool current_stream_is_closed_locked(void)
{
    if (!s_current_valid ||
        (s_current.source != PLAYBACK_SOURCE_PCM16_STREAM)) {
        return false;
    }

    audio_manager_pcm_stream_status_t manager_stream = {0};
    const esp_err_t result = audio_manager_pcm_stream_get_status(
        s_current.request.request_id,
        &manager_stream);
    if (result != ESP_OK) {
        return true;
    }
    s_current.stream.pcm_samples_accepted = manager_stream.accepted_samples;
    s_current.stream.pcm_samples_played = manager_stream.played_samples;
    s_current.stream.ingress_queue_high_water = manager_stream.high_water_samples;
    s_current.stream.ingress_full_count = manager_stream.full_count;
    s_current.stream.starvation_count = manager_stream.starvation_count;
    return !manager_stream.active;
}

static void playback_arbiter_task(void *arg)
{
    (void)arg;

    if (take_lock()) {
        sync_status_locked(AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE, ESP_OK);
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
                sync_status_locked(AUDIO_MANAGER_PLAYBACK_ARBITER_ERROR, status_ret);
                ++s_status.failed_count;
                xSemaphoreGive(s_lock);
            }
            vTaskDelay(pdMS_TO_TICKS(PLAYBACK_ARBITER_POLL_MS));
            continue;
        }

        bool do_abort = false;
        bool do_stop = false;
        bool do_start = false;
        playback_slot_t action_slot = {0};

        if (take_lock()) {
            if (s_current_valid) {
                update_stream_metrics_locked(&s_current);
                const bool stream_closed = current_stream_is_closed_locked();
                const bool terminal_requested =
                    s_current.cancel_requested ||
                    s_current.preempt_requested ||
                    s_current.failure_requested;

                if (terminal_requested) {
                    action_slot = s_current;
                    if (s_current.source == PLAYBACK_SOURCE_PCM16_STREAM) {
                        do_abort = true;
                    }
                    if (s_current.start_submitted) {
                        do_stop = true;
                        if ((s_current.source == PLAYBACK_SOURCE_PCM16_STREAM) &&
                            stream_closed &&
                            (manager.state != AUDIO_MANAGER_STATE_PLAYBACK)) {
                            finish_current_locked(ESP_OK);
                        } else if ((s_current.source == PLAYBACK_SOURCE_WAV) &&
                                   (manager.state == AUDIO_MANAGER_STATE_IDLE)) {
                            finish_current_locked(ESP_OK);
                        }
                    } else {
                        /* No manager command owns I2S yet. The action below
                         * flushes any prepared PCM ring before the next turn. */
                        finish_current_locked(ESP_OK);
                    }
                } else if (s_current.start_submitted) {
                    if (manager.state == AUDIO_MANAGER_STATE_PLAYBACK) {
                        s_current.stream.state =
                            s_current.stream_finish_requested
                                ? AUDIO_MANAGER_PLAYBACK_REQUEST_DRAINING
                                : AUDIO_MANAGER_PLAYBACK_REQUEST_ACTIVE;
                    } else if (s_current.source == PLAYBACK_SOURCE_PCM16_STREAM) {
                        /* IDLE is legitimate while the manager waits for the
                         * first bounded prefill; only a closed ring is terminal. */
                        if (stream_closed) {
                            finish_current_locked(manager.last_error);
                        }
                    } else if ((manager.state == AUDIO_MANAGER_STATE_IDLE) &&
                               (s_current.stream.state !=
                                AUDIO_MANAGER_PLAYBACK_REQUEST_STARTING)) {
                        finish_current_locked(manager.last_error);
                    }
                }

                if (s_current_valid && !s_current.start_submitted &&
                    !s_current.cancel_requested &&
                    !s_current.preempt_requested &&
                    !s_current.failure_requested &&
                    (manager.state == AUDIO_MANAGER_STATE_IDLE)) {
                    action_slot = s_current;
                    do_start = true;
                    s_current.stream.state =
                        AUDIO_MANAGER_PLAYBACK_REQUEST_STARTING;
                }
            }

            if (!s_current_valid && s_pending_valid &&
                (manager.state == AUDIO_MANAGER_STATE_IDLE)) {
                promote_pending_locked();
            }

            if (!s_current_valid && !s_pending_valid &&
                (manager.state == AUDIO_MANAGER_STATE_IDLE)) {
                sync_status_locked(AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE, ESP_OK);
            } else {
                sync_status_locked(visible_state_locked(), s_status.last_error);
            }
            xSemaphoreGive(s_lock);
        }

        if (do_abort &&
            (action_slot.source == PLAYBACK_SOURCE_PCM16_STREAM)) {
            (void)audio_manager_pcm_stream_abort(action_slot.request.request_id);
        }
        if (do_stop) {
            const esp_err_t ret = audio_manager_stop_playback();
            if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
                ESP_LOGW(TAG, "cooperative playback stop failed: %s",
                         esp_err_to_name(ret));
            }
        }

        if (do_start) {
            const esp_err_t ret =
                (action_slot.source == PLAYBACK_SOURCE_PCM16_STREAM)
                    ? audio_manager_pcm_stream_start(action_slot.request.request_id)
                    : audio_manager_play_wav(action_slot.path);
            if (take_lock()) {
                if (slot_has_request_id(&s_current,
                                        s_current_valid,
                                        action_slot.request.request_id)) {
                    if (ret == ESP_OK) {
                        s_current.start_submitted = true;
                        s_current.stream.state =
                            AUDIO_MANAGER_PLAYBACK_REQUEST_STARTING;
                        ESP_LOGI(TAG,
                                 "accepted request=%u client=%s priority=%u source=%s",
                                 (unsigned)action_slot.request.request_id,
                                 audio_manager_client_to_string(
                                     action_slot.request.client),
                                 (unsigned)action_slot.request.priority,
                                 (action_slot.source == PLAYBACK_SOURCE_PCM16_STREAM)
                                     ? "pcm16_stream"
                                     : "wav");
                    } else if (ret == ESP_ERR_INVALID_STATE) {
                        /* A legacy caller may have won after the copied
                         * status snapshot. Keep the request/ring and retry. */
                        s_current.stream.state =
                            AUDIO_MANAGER_PLAYBACK_REQUEST_PENDING;
                    } else {
                        s_current.failure_requested = true;
                        s_current.requested_failure = ret;
                    }
                    sync_status_locked(visible_state_locked(), ret);
                }
                xSemaphoreGive(s_lock);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PLAYBACK_ARBITER_POLL_MS));
    }
}

esp_err_t audio_manager_playback_arbiter_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(s_terminal_history, 0, sizeof(s_terminal_history));
    s_terminal_next = 0U;
    s_status.state = AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE;
    s_status.last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t audio_manager_playback_arbiter_start(void)
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

    if (xTaskCreate(playback_arbiter_task,
                    PLAYBACK_ARBITER_TASK_NAME,
                    PLAYBACK_ARBITER_TASK_STACK,
                    NULL,
                    PLAYBACK_ARBITER_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        if (take_lock()) {
            s_start_waiter = NULL;
            xSemaphoreGive(s_lock);
        }
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (ulTaskNotifyTake(pdTRUE,
                         pdMS_TO_TICKS(PLAYBACK_ARBITER_START_MS)) == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_manager_playback_arbiter_submit_wav(
    const audio_manager_request_t *request,
    const char *path)
{
    if ((s_lock == NULL) || (s_task == NULL) ||
        (request == NULL) || (path == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((audio_manager_request_validate(request) != ESP_OK) ||
        (request->resource != AUDIO_MANAGER_RESOURCE_PLAYBACK) ||
        (strnlen(path, AUDIO_MANAGER_WAV_PATH_MAX_BYTES) >=
         AUDIO_MANAGER_WAV_PATH_MAX_BYTES)) {
        return ESP_ERR_INVALID_ARG;
    }

    playback_slot_t incoming = {
        .request = *request,
        .source = PLAYBACK_SOURCE_WAV,
        .stream = {
            .request_id = request->request_id,
            .state = AUDIO_MANAGER_PLAYBACK_REQUEST_PENDING,
            .result = ESP_OK,
        },
    };
    memcpy(incoming.path, path, strlen(path) + 1U);

    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = submit_slot_locked(&incoming);
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t audio_manager_playback_arbiter_submit_pcm16_stream(
    const audio_manager_request_t *request)
{
    if ((s_lock == NULL) || (s_task == NULL) || (request == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((audio_manager_request_validate(request) != ESP_OK) ||
        (request->resource != AUDIO_MANAGER_RESOURCE_PLAYBACK)) {
        return ESP_ERR_INVALID_ARG;
    }

    playback_slot_t incoming = {
        .request = *request,
        .source = PLAYBACK_SOURCE_PCM16_STREAM,
        .stream = {
            .request_id = request->request_id,
            .state = AUDIO_MANAGER_PLAYBACK_REQUEST_PENDING,
            .result = ESP_OK,
        },
    };

    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if ((s_current_valid &&
         (s_current.source == PLAYBACK_SOURCE_PCM16_STREAM)) ||
        (s_pending_valid &&
         (s_pending.source == PLAYBACK_SOURCE_PCM16_STREAM))) {
        ++s_status.rejected_count;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t prepare_result = audio_manager_pcm_stream_prepare(
        request->request_id);
    if (prepare_result != ESP_OK) {
        ++s_status.rejected_count;
        xSemaphoreGive(s_lock);
        return prepare_result;
    }

    const esp_err_t submit_result = submit_slot_locked(&incoming);
    if (submit_result != ESP_OK) {
        (void)audio_manager_pcm_stream_abort(request->request_id);
    }
    xSemaphoreGive(s_lock);
    return submit_result;
}

esp_err_t audio_manager_playback_arbiter_write_pcm16(
    uint32_t request_id,
    const int16_t *samples,
    size_t sample_count)
{
    if ((s_lock == NULL) || (s_task == NULL) || (request_id == 0U) ||
        (samples == NULL) || (sample_count == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    playback_slot_t *slot = NULL;
    const bool valid = request_is_stream_locked(request_id, &slot) &&
                       !slot->stream_finish_requested &&
                       !slot->cancel_requested &&
                       !slot->preempt_requested &&
                       !slot->failure_requested;
    xSemaphoreGive(s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = audio_manager_pcm_stream_write(
        request_id,
        samples,
        sample_count);
    if (take_lock()) {
        if (request_is_stream_locked(request_id, &slot)) {
            update_stream_metrics_locked(slot);
            sync_status_locked(visible_state_locked(), result);
        }
        xSemaphoreGive(s_lock);
    }
    return result;
}

esp_err_t audio_manager_playback_arbiter_finish_pcm16_stream(
    uint32_t request_id)
{
    if ((s_lock == NULL) || (s_task == NULL) || (request_id == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    playback_slot_t *slot = NULL;
    if (!request_is_stream_locked(request_id, &slot) ||
        slot->stream_finish_requested || slot->cancel_requested ||
        slot->preempt_requested || slot->failure_requested) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    slot->stream_finish_requested = true;
    slot->stream.state = AUDIO_MANAGER_PLAYBACK_REQUEST_DRAINING;
    xSemaphoreGive(s_lock);

    const esp_err_t result = audio_manager_pcm_stream_finish(request_id);
    if (result != ESP_OK) {
        (void)audio_manager_playback_arbiter_fail_pcm16_stream(
            request_id,
            result);
    }
    return result;
}

static esp_err_t mark_stream_terminal_request(uint32_t request_id,
                                              bool failed,
                                              esp_err_t error)
{
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (slot_has_request_id(&s_pending, s_pending_valid, request_id) &&
        (s_pending.source == PLAYBACK_SOURCE_PCM16_STREAM)) {
        update_stream_metrics_locked(&s_pending);
        store_terminal_locked(
            &s_pending,
            failed ? AUDIO_MANAGER_PLAYBACK_REQUEST_FAILED
                   : AUDIO_MANAGER_PLAYBACK_REQUEST_CANCELLED,
            failed ? error : ESP_OK);
        clear_slot(&s_pending);
        s_pending_valid = false;
        sync_status_locked(visible_state_locked(), failed ? error : ESP_OK);
        xSemaphoreGive(s_lock);
        (void)audio_manager_pcm_stream_abort(request_id);
        return ESP_OK;
    }

    if (slot_has_request_id(&s_current, s_current_valid, request_id) &&
        (s_current.source == PLAYBACK_SOURCE_PCM16_STREAM)) {
        if (failed) {
            s_current.failure_requested = true;
            s_current.requested_failure = error;
        } else {
            s_current.cancel_requested = true;
        }
        sync_status_locked(visible_state_locked(), ESP_OK);
        xSemaphoreGive(s_lock);
        (void)audio_manager_pcm_stream_abort(request_id);
        return ESP_OK;
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_manager_playback_arbiter_fail_pcm16_stream(
    uint32_t request_id,
    esp_err_t error)
{
    if ((request_id == 0U) || (error == ESP_OK)) {
        return ESP_ERR_INVALID_ARG;
    }
    return mark_stream_terminal_request(request_id, true, error);
}

esp_err_t audio_manager_playback_arbiter_cancel(uint32_t request_id)
{
    if ((s_lock == NULL) || (s_task == NULL) || (request_id == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (slot_has_request_id(&s_pending, s_pending_valid, request_id)) {
        const bool stream =
            (s_pending.source == PLAYBACK_SOURCE_PCM16_STREAM);
        if (stream) {
            update_stream_metrics_locked(&s_pending);
        }
        store_terminal_locked(&s_pending,
                              AUDIO_MANAGER_PLAYBACK_REQUEST_CANCELLED,
                              ESP_OK);
        clear_slot(&s_pending);
        s_pending_valid = false;
        sync_status_locked(visible_state_locked(), ESP_OK);
        xSemaphoreGive(s_lock);
        if (stream) {
            (void)audio_manager_pcm_stream_abort(request_id);
        }
        return ESP_OK;
    }

    if (slot_has_request_id(&s_current, s_current_valid, request_id)) {
        const bool stream =
            (s_current.source == PLAYBACK_SOURCE_PCM16_STREAM);
        s_current.cancel_requested = true;
        sync_status_locked(visible_state_locked(), ESP_OK);
        xSemaphoreGive(s_lock);
        if (stream) {
            (void)audio_manager_pcm_stream_abort(request_id);
        }
        return ESP_OK;
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_manager_playback_arbiter_get_request_status(
    uint32_t request_id,
    audio_manager_playback_request_status_t *status)
{
    if ((s_lock == NULL) || (status == NULL) || (request_id == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (slot_has_request_id(&s_current, s_current_valid, request_id)) {
        update_stream_metrics_locked(&s_current);
        *status = s_current.stream;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (slot_has_request_id(&s_pending, s_pending_valid, request_id)) {
        update_stream_metrics_locked(&s_pending);
        *status = s_pending.stream;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    for (size_t index = 0U;
         index < PLAYBACK_ARBITER_TERMINAL_HISTORY_LENGTH;
         ++index) {
        if (s_terminal_history[index].request_id == request_id) {
            *status = s_terminal_history[index];
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t audio_manager_playback_arbiter_get_status(
    audio_manager_playback_arbiter_status_t *status)
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

const char *audio_manager_playback_arbiter_state_to_string(
    audio_manager_playback_arbiter_state_t state)
{
    switch (state) {
        case AUDIO_MANAGER_PLAYBACK_ARBITER_UNINITIALIZED: return "UNINITIALIZED";
        case AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE: return "IDLE";
        case AUDIO_MANAGER_PLAYBACK_ARBITER_STARTING: return "STARTING";
        case AUDIO_MANAGER_PLAYBACK_ARBITER_ACTIVE: return "ACTIVE";
        case AUDIO_MANAGER_PLAYBACK_ARBITER_PREEMPTING: return "PREEMPTING";
        case AUDIO_MANAGER_PLAYBACK_ARBITER_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
