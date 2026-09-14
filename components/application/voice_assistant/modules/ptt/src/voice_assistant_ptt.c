#include "voice_assistant_ptt.h"

#include <string.h>

#include "esp_log.h"
#include "app_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "voice_assistant.h"
#include "voice_assistant_downlink.h"
#include "voice_assistant_playback_control.h"

#define PTT_TASK_NAME                 "voice_ptt"
/* PTT now also handles the bounded post-abort transport-fence path. Keep the
 * policy worker on an 8 KiB Internal-RAM stack so that path retains margin. */
#define PTT_TASK_STACK_BYTES          8192U
#define PTT_TASK_PRIORITY             4U
#define PTT_QUEUE_LENGTH              6U
#define PTT_LOCK_TIMEOUT_MS           100U
#define PTT_TASK_START_TIMEOUT_MS     2000U
#define PTT_POLL_MS                   50U
#define PTT_ARMING_TIMEOUT_MS         45000U

typedef enum {
    PTT_COMMAND_PRESS = 0,
    PTT_COMMAND_RELEASE,
    PTT_COMMAND_CANCEL,
} ptt_command_type_t;

typedef struct {
    ptt_command_type_t type;
    uint32_t generation;
} ptt_command_t;

static const char *const TAG = "VOICE_PTT";

static SemaphoreHandle_t s_lock = NULL;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static TaskHandle_t s_start_waiter = NULL;
static TickType_t s_arming_started = 0U;
static bool s_command_pending = false;
/* A physical RELEASE must not be discarded just because its matching PRESS is
 * still waiting in the policy queue. The release is queued after that press
 * and prevents a fast tap from becoming an indefinitely authorized capture. */
static bool s_release_queued = false;

static voice_assistant_ptt_status_t s_status = {
    .state = VOICE_ASSISTANT_PTT_UNINITIALIZED,
    .last_error = ESP_OK,
};
static voice_assistant_ptt_status_callback_t s_callback = NULL;
static void *s_callback_context = NULL;

static bool ptt_take_lock(void)
{
    return (s_lock != NULL) &&
           (xSemaphoreTake(s_lock, pdMS_TO_TICKS(PTT_LOCK_TIMEOUT_MS)) == pdTRUE);
}

static void ptt_publish(void)
{
    voice_assistant_ptt_status_t snapshot = {0};
    voice_assistant_ptt_status_callback_t callback = NULL;
    void *context = NULL;

    if (!ptt_take_lock()) {
        APP_LOGW(TAG, STATUS_PUBLISH_SKIPPED_LOCK_619C7661, "status publish skipped: lock timeout");
        return;
    }
    snapshot = s_status;
    callback = s_callback;
    context = s_callback_context;
    xSemaphoreGive(s_lock);

    if (callback != NULL) {
        callback(&snapshot, context);
    }
}

static void ptt_set_status(
    voice_assistant_ptt_state_t state,
    bool pressed,
    bool capture_authorized,
    uint32_t session_generation,
    esp_err_t error)
{
    voice_assistant_ptt_state_t previous = VOICE_ASSISTANT_PTT_UNINITIALIZED;
    uint32_t generation = 0U;

    if (!ptt_take_lock()) {
        APP_LOGE(TAG, STATE_TRANSITION_DROPPED_LOC_1BA6E5F7, "state transition dropped: lock timeout");
        return;
    }
    previous = s_status.state;
    s_status.state = state;
    s_status.pressed = pressed;
    s_status.capture_authorized = capture_authorized;
    s_status.session_generation = session_generation;
    s_status.last_error = error;
    generation = s_status.ptt_generation;
    xSemaphoreGive(s_lock);

    APP_LOGI(TAG, STATE_S_S_PTT_GENERATION_F818D0F7,
             "state %s -> %s ptt_generation=%u session_generation=%u pressed=%s authorized=%s error=%s",
             voice_assistant_ptt_state_to_string(previous),
             voice_assistant_ptt_state_to_string(state),
             (unsigned)generation,
             (unsigned)session_generation,
             pressed ? "yes" : "no",
             capture_authorized ? "yes" : "no",
             esp_err_to_name(error));
    ptt_publish();
}

static void ptt_finish_command(ptt_command_type_t type)
{
    if (!ptt_take_lock()) {
        return;
    }
    if (type == PTT_COMMAND_RELEASE) {
        s_release_queued = false;
    }
    /* A matching RELEASE may already be queued behind the command that just
     * completed. Keep producer serialization active until it is consumed. */
    s_command_pending = s_release_queued;
    xSemaphoreGive(s_lock);
}

static bool ptt_release_is_queued(void)
{
    bool queued = false;

    if (ptt_take_lock()) {
        queued = s_release_queued;
        xSemaphoreGive(s_lock);
    }

    return queued;
}

static void ptt_prepare_and_authorize(
    uint32_t ptt_generation,
    uint32_t session_generation)
{
    ptt_set_status(VOICE_ASSISTANT_PTT_SUSPENDING_PLAYBACK,
                   true,
                   false,
                   session_generation,
                   ESP_OK);
    const esp_err_t prepare_ret = voice_assistant_playback_prepare_ptt(
        ptt_generation,
        session_generation);
    if (prepare_ret != ESP_OK) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       session_generation,
                       prepare_ret);
        return;
    }

    if (ptt_release_is_queued()) {
        const esp_err_t restore_ret =
            voice_assistant_playback_cancel_unstarted(ptt_generation);
        ptt_set_status(VOICE_ASSISTANT_PTT_RELEASED,
                       false,
                       false,
                       session_generation,
                       (restore_ret == ESP_ERR_INVALID_STATE)
                           ? ESP_OK
                           : restore_ret);
        APP_LOGI(TAG, PRESS_RELEASED_DURING_AUDIO__0F290C31,
                 "press released during audio suspension generation=%u restore=%s",
                 (unsigned)ptt_generation,
                 esp_err_to_name(restore_ret));
        return;
    }

    ptt_set_status(VOICE_ASSISTANT_PTT_AUTHORIZED,
                   true,
                   true,
                   session_generation,
                   ESP_OK);
}

/* A downlink-local abort can leave raw old WebSocket frames in flight. Do not
 * authorize capture on that client generation: reserve a replacement before
 * the lifecycle worker stops/drains/restarts the transport. */
static esp_err_t ptt_rotate_response_transport(
    uint32_t ptt_generation,
    const voice_assistant_status_t *voice)
{
    if ((voice == NULL) ||
        (voice->state != VOICE_ASSISTANT_STATE_READY) ||
        !voice_assistant_downlink_transport_fence_required(
            voice->session_generation)) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t rotate_ret = voice_assistant_rotate_session();
    if (rotate_ret != ESP_OK) {
        return rotate_ret;
    }

    voice_assistant_status_t updated = {0};
    const esp_err_t status_ret = voice_assistant_get_status(&updated);
    const uint32_t replacement_generation =
        (status_ret == ESP_OK) ? updated.session_generation :
        voice->session_generation;
    s_arming_started = xTaskGetTickCount();
    ptt_set_status(VOICE_ASSISTANT_PTT_ARMING_SESSION,
                   true,
                   false,
                   replacement_generation,
                   ESP_OK);
    APP_LOGI(TAG, RESPONSE_TRANSPORT_FENCE_ARMED_550BDF62,
             "press waiting for post-abort transport fence ptt_generation=%u old_generation=%u replacement_generation=%u",
             (unsigned)ptt_generation,
             (unsigned)voice->session_generation,
             (unsigned)replacement_generation);
    return ESP_OK;
}

static void ptt_reconcile_voice_state(void)
{
    voice_assistant_ptt_status_t ptt = {0};
    voice_assistant_status_t voice = {0};

    if (!ptt_take_lock()) {
        return;
    }
    ptt = s_status;
    xSemaphoreGive(s_lock);

    if ((ptt.state != VOICE_ASSISTANT_PTT_ARMING_SESSION) &&
        (ptt.state != VOICE_ASSISTANT_PTT_CANCEL_PENDING)) {
        return;
    }

    const esp_err_t get_ret = voice_assistant_get_status(&voice);
    if (get_ret != ESP_OK) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       ptt.session_generation,
                       get_ret);
        return;
    }

    if (ptt.state == VOICE_ASSISTANT_PTT_CANCEL_PENDING) {
        if (voice.state == VOICE_ASSISTANT_STATE_READY) {
            /* A boot-owned session remains available between PTT turns. If a
             * press is released while that session is reconnecting, READY
             * means only that pending turn is cancelled; stopping the session
             * here would turn a transient transport loss into a second-press
             * requirement. */
            ptt_set_status(VOICE_ASSISTANT_PTT_RELEASED,
                           false,
                           false,
                           voice.session_generation,
                           ESP_OK);
            return;
        }
        if (voice.state == VOICE_ASSISTANT_STATE_IDLE) {
            ptt_set_status(VOICE_ASSISTANT_PTT_IDLE,
                           false,
                           false,
                           voice.session_generation,
                           ESP_OK);
            return;
        }
        if (voice.state == VOICE_ASSISTANT_STATE_ERROR) {
            ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                           false,
                           false,
                           voice.session_generation,
                           voice.last_error == ESP_OK ? ESP_FAIL : voice.last_error);
        }
        return;
    }

    if (voice.state == VOICE_ASSISTANT_STATE_READY) {
        if (ptt.pressed) {
            if (voice_assistant_downlink_transport_fence_required(
                    voice.session_generation)) {
                const esp_err_t rotate_ret = ptt_rotate_response_transport(
                    ptt.ptt_generation, &voice);
                if (rotate_ret != ESP_OK) {
                    ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                                   false,
                                   false,
                                   voice.session_generation,
                                   rotate_ret);
                }
                return;
            }
            ptt_prepare_and_authorize(
                ptt.ptt_generation,
                voice.session_generation);
        } else {
            ptt_set_status(VOICE_ASSISTANT_PTT_CANCEL_PENDING,
                           false,
                           false,
                           voice.session_generation,
                           ESP_OK);
        }
        return;
    }

    if (voice.state == VOICE_ASSISTANT_STATE_ERROR) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       voice.session_generation,
                       voice.last_error == ESP_OK ? ESP_FAIL : voice.last_error);
        return;
    }

    /* A retained press that requested recovery has no session generation until
     * cleanup reaches IDLE. Start one fresh session and continue arming the
     * same physical press instead of requiring a second button press. */
    if ((voice.state == VOICE_ASSISTANT_STATE_IDLE) &&
        (ptt.session_generation == 0U)) {
        const esp_err_t begin_ret = voice_assistant_begin_session();
        if (begin_ret != ESP_OK) {
            ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                           false,
                           false,
                           voice.session_generation,
                           begin_ret);
            return;
        }

        voice_assistant_status_t updated = {0};
        const esp_err_t updated_ret = voice_assistant_get_status(&updated);
        ptt_set_status(VOICE_ASSISTANT_PTT_ARMING_SESSION,
                       true,
                       false,
                       (updated_ret == ESP_OK) ?
                           updated.session_generation : voice.session_generation,
                       ESP_OK);
        APP_LOGI(TAG, RECOVERY_STARTED_A_FRESH_SES_680437F9, "recovery started a fresh session while press remains held");
        return;
    }

    if ((xTaskGetTickCount() - s_arming_started) >=
        pdMS_TO_TICKS(PTT_ARMING_TIMEOUT_MS)) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       voice.session_generation,
                       ESP_ERR_TIMEOUT);
    }
}

static void ptt_handle_press(const ptt_command_t *command)
{
    voice_assistant_status_t voice = {0};
    const esp_err_t get_ret = voice_assistant_get_status(&voice);
    if (get_ret != ESP_OK) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR, false, false, 0U, get_ret);
        return;
    }

    /* The physical button was released before this queued PRESS reached the
     * policy task. Its FIFO RELEASE will follow, so never briefly authorize
     * microphone capture for a completed tap. */
    if (ptt_release_is_queued()) {
        ptt_set_status(VOICE_ASSISTANT_PTT_RELEASED,
                       false,
                       false,
                       voice.session_generation,
                       ESP_OK);
        APP_LOGI(TAG, PRESS_CANCELLED_BEFORE_PROCE_A90816FE,
                 "press cancelled before processing generation=%u",
                 (unsigned)command->generation);
        return;
    }

    /* PTT interruption is a local product-policy path. Transfer any retained
     * local suspension to this press, then ask the downlink owner to terminate
     * the old non-seekable response before capture authorization. */
    if (voice_assistant_downlink_is_busy()) {
        (void)voice_assistant_playback_supersede_ptt(
            command->generation,
            voice.session_generation);
        const esp_err_t interrupt_ret =
            voice_assistant_downlink_interrupt_active_response();
        if (interrupt_ret != ESP_OK) {
            ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                           false,
                           false,
                           voice.session_generation,
                           interrupt_ret);
            return;
        }
        APP_LOGI(TAG, PRIOR_RESPONSE_INTERRUPTED_RETA_3E30133F,
                 "prior response interrupted; retained press generation=%u",
                 (unsigned)command->generation);
    }

    /* The interrupt waits for downlink cleanup, not for WebSocket teardown.
     * Refresh the lifecycle snapshot before considering READY, then reserve a
     * new transport generation if this response marked the hard fence. */
    const esp_err_t refreshed_ret = voice_assistant_get_status(&voice);
    if (refreshed_ret != ESP_OK) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       0U,
                       refreshed_ret);
        return;
    }
    if ((voice.state == VOICE_ASSISTANT_STATE_READY) &&
        voice_assistant_downlink_transport_fence_required(
            voice.session_generation)) {
        const esp_err_t rotate_ret = ptt_rotate_response_transport(
            command->generation, &voice);
        if (rotate_ret != ESP_OK) {
            ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                           false,
                           false,
                           voice.session_generation,
                           rotate_ret);
        }
        return;
    }

    if (voice.state == VOICE_ASSISTANT_STATE_READY) {
        ptt_prepare_and_authorize(
            command->generation,
            voice.session_generation);
        return;
    }

    if (voice.state == VOICE_ASSISTANT_STATE_ERROR) {
        const esp_err_t recover_ret = voice_assistant_recover();
        if (recover_ret != ESP_OK) {
            ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                           false,
                           false,
                           voice.session_generation,
                           recover_ret);
            return;
        }
        s_arming_started = xTaskGetTickCount();
        ptt_set_status(VOICE_ASSISTANT_PTT_ARMING_SESSION,
                       true,
                       false,
                       0U,
                       ESP_OK);
        APP_LOGI(TAG, PRESS_RETAINED_THROUGH_BOUND_7A551615,
                 "press retained through bounded recovery generation=%u",
                 (unsigned)command->generation);
        return;
    }

    if (voice.state == VOICE_ASSISTANT_STATE_CONNECTING) {
        s_arming_started = xTaskGetTickCount();
        ptt_set_status(VOICE_ASSISTANT_PTT_ARMING_SESSION,
                       true,
                       false,
                       voice.session_generation,
                       ESP_OK);
        APP_LOGI(TAG, PRESS_WAITING_FOR_BOOT_RECON_E714DFC0,
                 "press waiting for boot/reconnect session generation=%u",
                 (unsigned)voice.session_generation);
        return;
    }

    if (voice.state != VOICE_ASSISTANT_STATE_IDLE) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       voice.session_generation,
                       ESP_ERR_INVALID_STATE);
        return;
    }

    const esp_err_t begin_ret = voice_assistant_begin_session();
    if (begin_ret != ESP_OK) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       voice.session_generation,
                       begin_ret);
        return;
    }

    voice_assistant_status_t updated = {0};
    const esp_err_t updated_ret = voice_assistant_get_status(&updated);
    const uint32_t session_generation =
        (updated_ret == ESP_OK) ? updated.session_generation : voice.session_generation;

    s_arming_started = xTaskGetTickCount();
    ptt_set_status(VOICE_ASSISTANT_PTT_ARMING_SESSION,
                   true,
                   false,
                   session_generation,
                   ESP_OK);
    APP_LOGI(TAG, PRESS_ARMED_GENERATION_U_C92011BD, "press armed generation=%u", (unsigned)command->generation);
}

static void ptt_handle_release(void)
{
    voice_assistant_ptt_status_t current = {0};
    if (!ptt_take_lock()) {
        return;
    }
    current = s_status;
    xSemaphoreGive(s_lock);

    switch (current.state) {
        case VOICE_ASSISTANT_PTT_ARMING_SESSION:
            ptt_set_status(VOICE_ASSISTANT_PTT_CANCEL_PENDING,
                           false,
                           false,
                           current.session_generation,
                           ESP_OK);
            break;
        case VOICE_ASSISTANT_PTT_AUTHORIZED:
            ptt_set_status(VOICE_ASSISTANT_PTT_RELEASED,
                           false,
                           false,
                           current.session_generation,
                           ESP_OK);
            /* If uplink has not crossed its start marker, this is a fast tap
             * and restores the temporary source. A real started turn retains
             * the context until its downlink terminal event. */
            (void)voice_assistant_playback_cancel_unstarted(
                current.ptt_generation);
            break;
        case VOICE_ASSISTANT_PTT_SUSPENDING_PLAYBACK:
            /* The PRESS handler observes the queued release after its bounded
             * suspension wait and restores before authorization. */
            break;
        case VOICE_ASSISTANT_PTT_IDLE:
        case VOICE_ASSISTANT_PTT_RELEASED:
            /* Repeated released-level samples are idempotent. */
            break;
        default:
            APP_LOGW(TAG, RELEASE_IGNORED_IN_STATE_S_B06D417A, "release ignored in state=%s",
                     voice_assistant_ptt_state_to_string(current.state));
            break;
    }
}

static void ptt_handle_cancel(void)
{
    voice_assistant_ptt_status_t current = {0};
    voice_assistant_status_t voice = {0};

    if (!ptt_take_lock()) {
        return;
    }
    current = s_status;
    xSemaphoreGive(s_lock);

    const esp_err_t get_ret = voice_assistant_get_status(&voice);
    if (get_ret != ESP_OK) {
        ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                       false,
                       false,
                       current.session_generation,
                       get_ret);
        return;
    }

    /* Resource-gate failures before capture start arrive here through the
     * uplink coordinator. Revoke the unstarted transaction so a successfully
     * suspended local source is not left paused indefinitely. If capture has
     * already started this returns INVALID_STATE and the normal downlink
     * terminal path remains the sole finalizer. */
    if ((current.state == VOICE_ASSISTANT_PTT_AUTHORIZED) ||
        (current.state == VOICE_ASSISTANT_PTT_SUSPENDING_PLAYBACK)) {
        (void)voice_assistant_playback_cancel_unstarted(
            current.ptt_generation);
    }

    if ((voice.state == VOICE_ASSISTANT_STATE_READY) ||
        (voice.state == VOICE_ASSISTANT_STATE_CONNECTING)) {
        /* Cancellation revokes the current PTT intent only. The production
         * session is deliberately long-lived and reconnects independently. */
        ptt_set_status(VOICE_ASSISTANT_PTT_CANCEL_PENDING,
                       false,
                       false,
                       voice.session_generation,
                       ESP_OK);
        return;
    }

    if ((voice.state == VOICE_ASSISTANT_STATE_CONNECTING) ||
        (current.state == VOICE_ASSISTANT_PTT_ARMING_SESSION) ||
        (current.state == VOICE_ASSISTANT_PTT_SUSPENDING_PLAYBACK)) {
        ptt_set_status(VOICE_ASSISTANT_PTT_CANCEL_PENDING,
                       false,
                       false,
                       voice.session_generation,
                       ESP_OK);
        return;
    }

    if (voice.state == VOICE_ASSISTANT_STATE_IDLE) {
        ptt_set_status(VOICE_ASSISTANT_PTT_IDLE,
                       false,
                       false,
                       voice.session_generation,
                       ESP_OK);
        return;
    }

    ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                   false,
                   false,
                   voice.session_generation,
                   ESP_ERR_INVALID_STATE);
}

static void ptt_task(void *argument)
{
    (void)argument;
    ptt_set_status(VOICE_ASSISTANT_PTT_IDLE, false, false, 0U, ESP_OK);

    TaskHandle_t waiter = NULL;
    if (ptt_take_lock()) {
        waiter = s_start_waiter;
        s_start_waiter = NULL;
        xSemaphoreGive(s_lock);
    }
    if (waiter != NULL) {
        xTaskNotifyGive(waiter);
    }

    for (;;) {
        ptt_command_t command = {0};
        if (xQueueReceive(s_queue, &command, pdMS_TO_TICKS(PTT_POLL_MS)) == pdTRUE) {
            switch (command.type) {
                case PTT_COMMAND_PRESS:
                    ptt_handle_press(&command);
                    break;
                case PTT_COMMAND_RELEASE:
                    ptt_handle_release();
                    break;
                case PTT_COMMAND_CANCEL:
                    ptt_handle_cancel();
                    break;
                default:
                    ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                                   false,
                                   false,
                                   0U,
                                   ESP_ERR_INVALID_ARG);
                    break;
            }
            ptt_finish_command(command.type);
        }
        ptt_reconcile_voice_state();
    }
}

static esp_err_t ptt_queue_command(ptt_command_type_t type)
{
    if ((s_lock == NULL) || (s_queue == NULL) || (s_task == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    ptt_command_t command = {.type = type, .generation = 0U};
    if (!ptt_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    const bool command_was_pending = s_command_pending;

    if (s_command_pending) {
        if (type != PTT_COMMAND_RELEASE) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }

        if (s_release_queued || !s_status.pressed) {
            /* Repeated GPIO samples of one released level are idempotent. */
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
    }

    if (type == PTT_COMMAND_PRESS) {
        if ((s_status.state != VOICE_ASSISTANT_PTT_IDLE) &&
            (s_status.state != VOICE_ASSISTANT_PTT_RELEASED) &&
            (s_status.state != VOICE_ASSISTANT_PTT_ERROR)) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        ++s_status.ptt_generation;
        if (s_status.ptt_generation == 0U) {
            s_status.ptt_generation = 1U;
        }
        command.generation = s_status.ptt_generation;
        s_status.pressed = true;
        s_status.capture_authorized = false;
    } else {
        if ((type == PTT_COMMAND_RELEASE) && !s_status.pressed) {
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
        command.generation = s_status.ptt_generation;
    }

    if (type == PTT_COMMAND_RELEASE) {
        s_release_queued = true;
    }
    s_command_pending = true;
    xSemaphoreGive(s_lock);

    if (xQueueSend(s_queue, &command, 0U) != pdTRUE) {
        if (ptt_take_lock()) {
            s_command_pending = command_was_pending;
            if (type == PTT_COMMAND_PRESS) {
                s_status.pressed = false;
            }
            if (type == PTT_COMMAND_RELEASE) {
                s_release_queued = false;
            }
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t voice_assistant_ptt_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_queue = xQueueCreate(PTT_QUEUE_LENGTH, sizeof(ptt_command_t));
    if (s_queue == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = VOICE_ASSISTANT_PTT_IDLE;
    s_status.last_error = ESP_OK;
    s_command_pending = false;
    s_release_queued = false;
    APP_LOGI(TAG, INITIALIZED_WITHOUT_GPIO_OWN_5DB50327, "initialized without GPIO ownership");
    return ESP_OK;
}

esp_err_t voice_assistant_ptt_start(void)
{
    if ((s_lock == NULL) || (s_queue == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ptt_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_task != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_start_waiter = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(s_lock);

    if (xTaskCreate(ptt_task,
                    PTT_TASK_NAME,
                    PTT_TASK_STACK_BYTES,
                    NULL,
                    PTT_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        if (ptt_take_lock()) {
            s_start_waiter = NULL;
            xSemaphoreGive(s_lock);
        }
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PTT_TASK_START_TIMEOUT_MS)) == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t voice_assistant_ptt_press(void)
{
    return ptt_queue_command(PTT_COMMAND_PRESS);
}

esp_err_t voice_assistant_ptt_release(void)
{
    return ptt_queue_command(PTT_COMMAND_RELEASE);
}

esp_err_t voice_assistant_ptt_cancel(void)
{
    return ptt_queue_command(PTT_COMMAND_CANCEL);
}

esp_err_t voice_assistant_ptt_register_status_callback(
    voice_assistant_ptt_status_callback_t callback,
    void *user_context)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ptt_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    s_callback = callback;
    s_callback_context = user_context;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_ptt_get_status(voice_assistant_ptt_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        memset(status, 0, sizeof(*status));
        status->state = VOICE_ASSISTANT_PTT_UNINITIALIZED;
        return ESP_ERR_INVALID_STATE;
    }
    if (!ptt_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *voice_assistant_ptt_state_to_string(voice_assistant_ptt_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_PTT_UNINITIALIZED:
            return "UNINITIALIZED";
        case VOICE_ASSISTANT_PTT_IDLE:
            return "IDLE";
        case VOICE_ASSISTANT_PTT_ARMING_SESSION:
            return "ARMING_SESSION";
        case VOICE_ASSISTANT_PTT_SUSPENDING_PLAYBACK:
            return "SUSPENDING_PLAYBACK";
        case VOICE_ASSISTANT_PTT_AUTHORIZED:
            return "AUTHORIZED";
        case VOICE_ASSISTANT_PTT_RELEASED:
            return "RELEASED";
        case VOICE_ASSISTANT_PTT_CANCEL_PENDING:
            return "CANCEL_PENDING";
        case VOICE_ASSISTANT_PTT_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
