#include "voice_assistant_ptt.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "voice_assistant.h"

#define PTT_TASK_NAME                 "voice_ptt"
#define PTT_TASK_STACK_BYTES          4096U
#define PTT_TASK_PRIORITY             4U
#define PTT_QUEUE_LENGTH              6U
#define PTT_LOCK_TIMEOUT_MS           100U
#define PTT_TASK_START_TIMEOUT_MS     2000U
#define PTT_POLL_MS                   50U
#define PTT_ARMING_TIMEOUT_MS         20000U

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
        ESP_LOGW(TAG, "status publish skipped: lock timeout");
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
        ESP_LOGE(TAG, "state transition dropped: lock timeout");
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

    ESP_LOGI(TAG,
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

static void ptt_finish_command(void)
{
    if (!ptt_take_lock()) {
        return;
    }
    s_command_pending = false;
    xSemaphoreGive(s_lock);
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
            const esp_err_t stop_ret = voice_assistant_end_session();
            if ((stop_ret != ESP_OK) && (stop_ret != ESP_ERR_INVALID_STATE)) {
                ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                               false,
                               false,
                               voice.session_generation,
                               stop_ret);
            }
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
            ptt_set_status(VOICE_ASSISTANT_PTT_AUTHORIZED,
                           true,
                           true,
                           voice.session_generation,
                           ESP_OK);
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

    if (voice.state == VOICE_ASSISTANT_STATE_READY) {
        ptt_set_status(VOICE_ASSISTANT_PTT_AUTHORIZED,
                       true,
                       true,
                       voice.session_generation,
                       ESP_OK);
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
        ptt_set_status(VOICE_ASSISTANT_PTT_CANCEL_PENDING,
                       false,
                       false,
                       voice.session_generation,
                       ESP_OK);
        ESP_LOGI(TAG,
                 "press requested bounded recovery generation=%u; press again after IDLE",
                 (unsigned)command->generation);
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
    ESP_LOGI(TAG, "press armed generation=%u", (unsigned)command->generation);
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
            break;
        default:
            ESP_LOGW(TAG, "release ignored in state=%s",
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

    if (voice.state == VOICE_ASSISTANT_STATE_READY) {
        const esp_err_t end_ret = voice_assistant_end_session();
        if (end_ret == ESP_OK) {
            ptt_set_status(VOICE_ASSISTANT_PTT_CANCEL_PENDING,
                           false,
                           false,
                           voice.session_generation,
                           ESP_OK);
        } else {
            ptt_set_status(VOICE_ASSISTANT_PTT_ERROR,
                           false,
                           false,
                           voice.session_generation,
                           end_ret);
        }
        return;
    }

    if ((voice.state == VOICE_ASSISTANT_STATE_CONNECTING) ||
        (current.state == VOICE_ASSISTANT_PTT_ARMING_SESSION)) {
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
            ptt_finish_command();
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

    if (s_command_pending) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
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
        command.generation = s_status.ptt_generation;
    }

    s_command_pending = true;
    xSemaphoreGive(s_lock);

    if (xQueueSend(s_queue, &command, 0U) != pdTRUE) {
        if (ptt_take_lock()) {
            s_command_pending = false;
            if (type == PTT_COMMAND_PRESS) {
                s_status.pressed = false;
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
    ESP_LOGI(TAG, "initialized without GPIO ownership");
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
