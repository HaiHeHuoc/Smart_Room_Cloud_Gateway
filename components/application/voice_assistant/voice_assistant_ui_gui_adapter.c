#include "voice_assistant_ui_gui_adapter.h"

#include <stdio.h>
#include <string.h>

#include "app_gui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "voice_assistant_ui_model.h"

#define VOICE_UI_DASHBOARD_RETURN_DELAY_US   (3LL * 1000LL * 1000LL)
#define VOICE_UI_ROUTE_RETRY_DELAY_US        (100LL * 1000LL)
#define VOICE_UI_ROUTE_RETRY_MAX_ATTEMPTS    10U
#define VOICE_UI_CALLBACK_LOCK_TIMEOUT_MS    50U

static const char *const TAG = "VOICE_UI_GUI";

static bool s_initialized = false;
static bool s_started = false;
static bool s_interaction_screen_owned = false;
static voice_assistant_ui_state_t s_previous_state = VOICE_ASSISTANT_UI_IDLE;
static SemaphoreHandle_t s_callback_lock = NULL;
static esp_timer_handle_t s_dashboard_return_timer = NULL;
static esp_timer_handle_t s_xiaozhi_open_retry_timer = NULL;
static uint32_t s_dashboard_retry_attempts = 0U;
static uint32_t s_xiaozhi_open_retry_attempts = 0U;

static bool gui_take_callback_lock(TickType_t wait_ticks)
{
    return (s_callback_lock != NULL) &&
           (xSemaphoreTake(s_callback_lock, wait_ticks) == pdTRUE);
}

static void gui_release_callback_lock(void)
{
    if (s_callback_lock != NULL) {
        xSemaphoreGive(s_callback_lock);
    }
}

static ui_xiaozhi_state_t gui_map_state(voice_assistant_ui_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_UI_CONNECTING:
            return UI_XIAOZHI_STATE_CONNECTING;
        case VOICE_ASSISTANT_UI_READY:
            return UI_XIAOZHI_STATE_READY;
        case VOICE_ASSISTANT_UI_LISTENING:
            return UI_XIAOZHI_STATE_LISTENING;
        case VOICE_ASSISTANT_UI_THINKING:
            return UI_XIAOZHI_STATE_PROCESSING;
        case VOICE_ASSISTANT_UI_SPEAKING:
            return UI_XIAOZHI_STATE_RESPONDING;
        case VOICE_ASSISTANT_UI_RECOVERING:
            return UI_XIAOZHI_STATE_RECOVERING;
        case VOICE_ASSISTANT_UI_ERROR:
            return UI_XIAOZHI_STATE_ERROR;
        case VOICE_ASSISTANT_UI_IDLE:
        default:
            return UI_XIAOZHI_STATE_DISCONNECTED;
    }
}

static esp_err_t gui_map_error(
    ui_xiaozhi_state_t gui_state,
    esp_err_t production_error)
{
    if (gui_state != UI_XIAOZHI_STATE_ERROR) {
        return ESP_OK;
    }

    return (production_error == ESP_OK) ? ESP_FAIL : production_error;
}

static void gui_copy_text(
    char destination[UI_XIAOZHI_TEXT_BUFFER_SIZE],
    const char *source,
    bool valid)
{
    destination[0] = '\0';
    if (!valid || (source == NULL)) {
        return;
    }

    (void)snprintf(
        destination,
        UI_XIAOZHI_TEXT_BUFFER_SIZE,
        "%s",
        source);
}

static bool gui_state_belongs_to_active_turn(voice_assistant_ui_state_t state)
{
    return (state == VOICE_ASSISTANT_UI_LISTENING) ||
           (state == VOICE_ASSISTANT_UI_THINKING) ||
           (state == VOICE_ASSISTANT_UI_SPEAKING) ||
           (state == VOICE_ASSISTANT_UI_RECOVERING) ||
           (state == VOICE_ASSISTANT_UI_ERROR);
}

static void gui_cancel_timer(esp_timer_handle_t timer, const char *name)
{
    if ((timer == NULL) || !esp_timer_is_active(timer)) {
        return;
    }

    const esp_err_t ret = esp_timer_stop(timer);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(
            TAG,
            "%s timer stop failed: %s",
            name,
            esp_err_to_name(ret));
    }
}

static void gui_cancel_dashboard_return_locked(void)
{
    gui_cancel_timer(s_dashboard_return_timer, "dashboard return");
    s_dashboard_retry_attempts = 0U;
}

static void gui_cancel_xiaozhi_open_retry_locked(void)
{
    gui_cancel_timer(s_xiaozhi_open_retry_timer, "Xiaozhi open retry");
    s_xiaozhi_open_retry_attempts = 0U;
}

static void gui_arm_retry_timer(
    esp_timer_handle_t timer,
    const char *name)
{
    if (timer == NULL) {
        return;
    }

    if (esp_timer_is_active(timer)) {
        const esp_err_t stop_ret = esp_timer_stop(timer);
        if ((stop_ret != ESP_OK) &&
            (stop_ret != ESP_ERR_INVALID_STATE)) {
            ESP_LOGW(
                TAG,
                "%s retry timer stop failed: %s",
                name,
                esp_err_to_name(stop_ret));
            return;
        }
    }

    const esp_err_t ret =
        esp_timer_start_once(timer, VOICE_UI_ROUTE_RETRY_DELAY_US);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "%s retry timer start failed: %s",
            name,
            esp_err_to_name(ret));
    }
}

static void gui_schedule_xiaozhi_open_retry_locked(void)
{
    if (s_xiaozhi_open_retry_attempts >=
        VOICE_UI_ROUTE_RETRY_MAX_ATTEMPTS) {
        ESP_LOGE(
            TAG,
            "Xiaozhi screen route retry exhausted after %u attempts",
            (unsigned)s_xiaozhi_open_retry_attempts);
        return;
    }

    s_xiaozhi_open_retry_attempts++;
    gui_arm_retry_timer(
        s_xiaozhi_open_retry_timer,
        "Xiaozhi open");
}

static esp_err_t gui_request_xiaozhi_screen_locked(bool force_queue)
{
    app_gui_screen_id_t current = APP_GUI_SCREEN_NONE;
    const esp_err_t get_ret = app_gui_get_screen_id(&current);

    if ((get_ret == ESP_OK) &&
        (current == APP_GUI_SCREEN_XIAOZHI) &&
        !force_queue) {
        gui_cancel_xiaozhi_open_retry_locked();
        return ESP_OK;
    }

    const esp_err_t ret =
        app_gui_request_screen(APP_GUI_SCREEN_XIAOZHI);

    if (ret == ESP_OK) {
        gui_cancel_xiaozhi_open_retry_locked();
    }
    else if (ret == ESP_ERR_TIMEOUT) {
        gui_schedule_xiaozhi_open_retry_locked();
    }
    else if (ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(
            TAG,
            "Xiaozhi screen request failed: %s",
            esp_err_to_name(ret));
    }

    return ret;
}

static void gui_xiaozhi_open_retry_timer_cb(void *argument)
{
    (void)argument;

    if (!gui_take_callback_lock(0U)) {
        gui_arm_retry_timer(
            s_xiaozhi_open_retry_timer,
            "Xiaozhi open lock contention");
        return;
    }

    if (!s_interaction_screen_owned ||
        !gui_state_belongs_to_active_turn(s_previous_state)) {
        gui_cancel_xiaozhi_open_retry_locked();
        gui_release_callback_lock();
        return;
    }

    (void)gui_request_xiaozhi_screen_locked(false);
    gui_release_callback_lock();
}

static void gui_schedule_dashboard_return_locked(void)
{
    if (s_dashboard_return_timer == NULL) {
        return;
    }

    gui_cancel_dashboard_return_locked();

    const esp_err_t ret = esp_timer_start_once(
        s_dashboard_return_timer,
        VOICE_UI_DASHBOARD_RETURN_DELAY_US);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dashboard return timer start failed: %s",
            esp_err_to_name(ret));
    }
}

static void gui_schedule_dashboard_retry_locked(void)
{
    if (s_dashboard_retry_attempts >=
        VOICE_UI_ROUTE_RETRY_MAX_ATTEMPTS) {
        ESP_LOGE(
            TAG,
            "dashboard return retry exhausted after %u attempts",
            (unsigned)s_dashboard_retry_attempts);
        return;
    }

    s_dashboard_retry_attempts++;
    gui_arm_retry_timer(
        s_dashboard_return_timer,
        "dashboard return");
}

static void gui_dashboard_return_timer_cb(void *argument)
{
    (void)argument;

    /* ESP timer callbacks must not block behind voice/network callbacks. */
    if (!gui_take_callback_lock(0U)) {
        gui_arm_retry_timer(
            s_dashboard_return_timer,
            "dashboard return lock contention");
        return;
    }

    if (!s_interaction_screen_owned ||
        (s_previous_state != VOICE_ASSISTANT_UI_READY)) {
        s_dashboard_retry_attempts = 0U;
        gui_release_callback_lock();
        return;
    }

    app_gui_screen_id_t current = APP_GUI_SCREEN_NONE;
    const esp_err_t get_ret = app_gui_get_screen_id(&current);
    if (get_ret != ESP_OK) {
        gui_schedule_dashboard_retry_locked();
        gui_release_callback_lock();
        return;
    }

    if (current != APP_GUI_SCREEN_XIAOZHI) {
        /* Another explicit route already owns the UI; do not override it. */
        s_interaction_screen_owned = false;
        s_dashboard_retry_attempts = 0U;
        gui_release_callback_lock();
        return;
    }

    const esp_err_t ret =
        app_gui_request_screen(APP_GUI_SCREEN_SENSOR_DASHBOARD);
    if (ret == ESP_OK) {
        s_interaction_screen_owned = false;
        s_dashboard_retry_attempts = 0U;
    }
    else if (ret == ESP_ERR_TIMEOUT) {
        gui_schedule_dashboard_retry_locked();
    }
    else if (ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(
            TAG,
            "dashboard return request failed: %s",
            esp_err_to_name(ret));
    }

    gui_release_callback_lock();
}

static void gui_apply_screen_policy_locked(voice_assistant_ui_state_t state)
{
    const voice_assistant_ui_state_t previous = s_previous_state;
    s_previous_state = state;

    /*
     * Only a real READY -> microphone-capture transition authorizes the
     * Xiaozhi interaction screen. Always queue this route even if Xiaozhi is
     * already the visible screen: it intentionally supersedes a dashboard
     * command that may have been queued by the 3-second timer just before PTT.
     */
    if ((previous == VOICE_ASSISTANT_UI_READY) &&
        (state == VOICE_ASSISTANT_UI_LISTENING)) {
        s_interaction_screen_owned = true;
        gui_cancel_dashboard_return_locked();
        (void)gui_request_xiaozhi_screen_locked(true);
        return;
    }

    if (gui_state_belongs_to_active_turn(state)) {
        gui_cancel_dashboard_return_locked();

        if (s_interaction_screen_owned) {
            (void)gui_request_xiaozhi_screen_locked(false);
        }
        return;
    }

    /*
     * After an authorized interaction settles to READY, arm the return even
     * if the queued Xiaozhi route has not been rendered yet. This closes the
     * fast-turn race where READY could arrive before the GUI task consumes
     * the earlier Xiaozhi command.
     */
    if (state == VOICE_ASSISTANT_UI_READY) {
        gui_cancel_xiaozhi_open_retry_locked();

        if (s_interaction_screen_owned &&
            (previous != VOICE_ASSISTANT_UI_READY)) {
            gui_schedule_dashboard_return_locked();
        }
        return;
    }

    /* Startup/reconnect status never claims the interaction screen. */
    if ((state == VOICE_ASSISTANT_UI_CONNECTING) ||
        (state == VOICE_ASSISTANT_UI_IDLE)) {
        gui_cancel_dashboard_return_locked();
        gui_cancel_xiaozhi_open_retry_locked();
        s_interaction_screen_owned = false;
    }
}

static void gui_model_callback(
    const voice_assistant_ui_model_t *model,
    void *user_context)
{
    (void)model;
    (void)user_context;

    if (!gui_take_callback_lock(
            pdMS_TO_TICKS(VOICE_UI_CALLBACK_LOCK_TIMEOUT_MS))) {
        ESP_LOGW(TAG, "voice GUI callback dropped: adapter lock timeout");
        return;
    }

    /*
     * Always consume the model's latest snapshot while the adapter callback is
     * serialized. A callback trigger can arrive from more than one producer;
     * re-reading here prevents an older trigger from regressing UI state/text.
     */
    voice_assistant_ui_model_t latest = {0};
    const esp_err_t get_ret = voice_assistant_ui_model_get(&latest);
    if (get_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "voice GUI snapshot read failed: %s",
            esp_err_to_name(get_ret));
        gui_release_callback_lock();
        return;
    }

    const ui_xiaozhi_state_t gui_state = gui_map_state(latest.state);
    ui_xiaozhi_status_t gui = {
        .state = gui_state,
        .listening_started_at_us = latest.listening_started_at_us,
        .listening_stopped_at_us = latest.listening_stopped_at_us,
        .last_error = gui_map_error(gui_state, latest.last_error),
        .user_text_truncated = latest.user_text_truncated,
        .assistant_text_truncated = latest.assistant_text_truncated,
    };

    gui_copy_text(gui.user_text, latest.user_text, latest.user_text_valid);
    gui_copy_text(
        gui.assistant_text,
        latest.assistant_text,
        latest.assistant_text_valid);

    const esp_err_t post_ret = app_gui_post_xiaozhi_status(&gui);
    if ((post_ret != ESP_OK) &&
        (post_ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(
            TAG,
            "voice GUI snapshot dropped state=%s error=%s",
            voice_assistant_ui_state_to_string(latest.state),
            esp_err_to_name(post_ret));
    }

    gui_apply_screen_policy_locked(latest.state);
    gui_release_callback_lock();
}

esp_err_t voice_assistant_ui_gui_adapter_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_callback_lock = xSemaphoreCreateMutex();
    if (s_callback_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t dashboard_timer_args = {
        .callback = gui_dashboard_return_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "xiaozhi_ui_return",
        .skip_unhandled_events = true,
    };

    esp_err_t ret = esp_timer_create(
        &dashboard_timer_args,
        &s_dashboard_return_timer);
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_callback_lock);
        s_callback_lock = NULL;
        return ret;
    }

    const esp_timer_create_args_t open_retry_timer_args = {
        .callback = gui_xiaozhi_open_retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "xiaozhi_ui_open",
        .skip_unhandled_events = true,
    };

    ret = esp_timer_create(
        &open_retry_timer_args,
        &s_xiaozhi_open_retry_timer);
    if (ret != ESP_OK) {
        (void)esp_timer_delete(s_dashboard_return_timer);
        s_dashboard_return_timer = NULL;
        vSemaphoreDelete(s_callback_lock);
        s_callback_lock = NULL;
        return ret;
    }

    s_previous_state = VOICE_ASSISTANT_UI_IDLE;
    s_interaction_screen_owned = false;
    s_dashboard_retry_attempts = 0U;
    s_xiaozhi_open_retry_attempts = 0U;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t voice_assistant_ui_gui_adapter_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    const esp_err_t ret = voice_assistant_ui_model_register_callback(
        gui_model_callback,
        NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    voice_assistant_ui_model_t model = {0};
    const esp_err_t get_ret = voice_assistant_ui_model_get(&model);
    if (get_ret != ESP_OK) {
        (void)voice_assistant_ui_model_register_callback(NULL, NULL);
        return get_ret;
    }

    if (!gui_take_callback_lock(
            pdMS_TO_TICKS(VOICE_UI_CALLBACK_LOCK_TIMEOUT_MS))) {
        (void)voice_assistant_ui_model_register_callback(NULL, NULL);
        return ESP_ERR_TIMEOUT;
    }

    s_previous_state = model.state;
    s_interaction_screen_owned = false;
    gui_release_callback_lock();

    s_started = true;
    gui_model_callback(&model, NULL);
    ESP_LOGI(TAG, "production voice GUI adapter started");
    return ESP_OK;
}
