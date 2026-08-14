/* Includes ----------------------------------------------------------------- */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ui_manager_lvgl.h"
#include "app_gui.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "lvgl.h"

#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#if !LV_USE_QRCODE
#error "Phase 6.4.3 requires CONFIG_LV_USE_QRCODE=y"
#endif

/* Macros ------------------------------------------------------------------- */
#define APP_GUI_WIFI_SCREEN_TIMEOUT_MS 10000U
#define APP_GUI_COMMAND_QUEUE_LENGTH 8U
#define APP_GUI_PROVISIONING_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_PROVISIONING_QR_QUEUE_LENGTH 1U
#define APP_GUI_WIFI_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_SENSOR_STATUS_QUEUE_LENGTH 5U
#define APP_GUI_AUDIO_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_CLOUD_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_UI_TASK_STACK_SIZE_BYTES   (24U * 1024U)
#define APP_GUI_TASK_PRIORITY              5U
#define APP_GUI_TASK_PERIOD_MS              33U
#define APP_GUI_STACK_LOG_PERIOD_MS         (60U * 1000U)
#define APP_GUI_STACK_WARNING_BYTES         (2U * 1024U)
#define APP_GUI_WIFI_VALUE_WIDTH_PX          100
#define APP_GUI_WIFI_VALUE_HEIGHT_PX          18
#define APP_GUI_WIFI_IP_VALUE_WIDTH_PX        120
#define APP_GUI_SENSOR_VALUE_WIDTH_PX          92
#define APP_GUI_SENSOR_VALUE_HEIGHT_PX         18

/*
 * The 73-byte Security 1 payload needs QR version 5 at LVGL's medium ECC:
 * 37 modules x 2 pixels = 74 pixels. A 92-pixel white container leaves a
 * 9-pixel quiet zone on each side and fills the remaining screen height.
 */
#define APP_GUI_PROVISIONING_QR_CONTAINER_X_PX     0
#define APP_GUI_PROVISIONING_QR_CONTAINER_Y_PX    36
#define APP_GUI_PROVISIONING_QR_CONTAINER_SIZE_PX 92
#define APP_GUI_PROVISIONING_QR_CANVAS_SIZE_PX    74
#define APP_GUI_PROVISIONING_TEXT_X_PX            94
#define APP_GUI_PROVISIONING_TEXT_WIDTH_PX      66
#define APP_GUI_PROVISIONING_INDICATOR_SIZE_PX   8
#define APP_GUI_PROVISIONING_INSTRUCTION_BUFFER_SIZE 96U

/* sensor_manager publishes this sentinel after a failed DHT22 read. */
#define APP_GUI_SENSOR_FAILED_VALUE             (-1.0f)

/* Logical landscape layout after LVGL rotates the 128x160 panel. */
#define APP_GUI_DASHBOARD_WIDTH_PX              160
#define APP_GUI_DASHBOARD_HEIGHT_PX             128
#define APP_GUI_DASHBOARD_HEADER_HEIGHT_PX       25
#define APP_GUI_DASHBOARD_COLUMN_X_PX            78
#define APP_GUI_DASHBOARD_MARGIN_PX               4
#define APP_GUI_DASHBOARD_RIGHT_X_PX             82
#define APP_GUI_DASHBOARD_RIGHT_WIDTH_PX         76
#define APP_GUI_DASHBOARD_STATUS_PANEL_HEIGHT_PX \
    (APP_GUI_DASHBOARD_HEIGHT_PX - \
     APP_GUI_DASHBOARD_HEADER_HEIGHT_PX - 1)
/*
 * Montserrat 10 has an 11-pixel line height. The 102-pixel status panel
 * therefore uses symmetric 12/11/12/11/12-pixel outer/inter-row gaps.
 */
#define APP_GUI_DASHBOARD_STATUS_LABEL_HEIGHT_PX 11
#define APP_GUI_DASHBOARD_STATUS_OUTER_GAP_PX    12
#define APP_GUI_DASHBOARD_STATUS_SMALL_GAP_PX    11
#define APP_GUI_DASHBOARD_STATUS_LARGE_GAP_PX    12
#define APP_GUI_DASHBOARD_STATUS_WIFI_Y_PX \
    (APP_GUI_DASHBOARD_HEADER_HEIGHT_PX + \
     APP_GUI_DASHBOARD_STATUS_OUTER_GAP_PX)
#define APP_GUI_DASHBOARD_STATUS_CLOUD_Y_PX \
    (APP_GUI_DASHBOARD_STATUS_WIFI_Y_PX + \
     APP_GUI_DASHBOARD_STATUS_LABEL_HEIGHT_PX + \
     APP_GUI_DASHBOARD_STATUS_SMALL_GAP_PX)
#define APP_GUI_DASHBOARD_STATUS_SENSOR_Y_PX \
    (APP_GUI_DASHBOARD_STATUS_CLOUD_Y_PX + \
     APP_GUI_DASHBOARD_STATUS_LABEL_HEIGHT_PX + \
     APP_GUI_DASHBOARD_STATUS_LARGE_GAP_PX)
#define APP_GUI_DASHBOARD_STATUS_AUDIO_Y_PX \
    (APP_GUI_DASHBOARD_STATUS_SENSOR_Y_PX + \
     APP_GUI_DASHBOARD_STATUS_LABEL_HEIGHT_PX + \
     APP_GUI_DASHBOARD_STATUS_SMALL_GAP_PX)

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    APP_GUI_COMMAND_SHOW_SCREEN = 0,
    APP_GUI_COMMAND_SHOW_RESET_RESULT
} app_gui_command_type_t;

typedef struct
{
    app_gui_command_type_t type;

    union
    {
        app_gui_screen_id_t screen_id;
        ui_reset_status_t reset_status;
    } payload;
} app_gui_command_t;

typedef struct
{
    bool payload_available;
    ui_provisioning_qr_payload_t payload;
} app_gui_provisioning_qr_message_t;

typedef struct
{
    lv_obj_t *provisioning_qr_container;
    lv_obj_t *provisioning_qr_code;
    lv_obj_t *provisioning_title_label;
    lv_obj_t *provisioning_instruction_label;
    lv_obj_t *provisioning_status_label;
    lv_obj_t *provisioning_state_indicator;
    lv_obj_t *wifi_mode_label;
    lv_obj_t *wifi_ssid_label;
    lv_obj_t *wifi_ip_label;
    lv_obj_t *sensor_temperature_label;
    lv_obj_t *sensor_humidity_label;
    lv_obj_t *sensor_audio_label;
    lv_obj_t *sensor_state_label;
    lv_obj_t *sensor_wifi_label;
    lv_obj_t *sensor_wifi_dot;
    lv_obj_t *sensor_cloud_label;
    lv_obj_t *sensor_cloud_dot;
    lv_obj_t *reset_status_label;
    lv_obj_t *reset_detail_label;
    lv_obj_t *reset_state_indicator;
} app_gui_widget_refs_t;

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_GUI";

/* Static Variables --------------------------------------------------------- */
/* GUI task communication and active-screen tracking. */
static QueueHandle_t s_command_queue = NULL;
static QueueHandle_t s_provisioning_status_queue = NULL;
static QueueHandle_t s_provisioning_qr_queue = NULL;
static QueueHandle_t s_wifi_status_queue = NULL;
static QueueHandle_t s_sensor_status_queue = NULL;
static QueueHandle_t s_audio_status_queue = NULL;
static QueueHandle_t s_cloud_status_queue = NULL;
static TaskHandle_t s_ui_task_handle = NULL;
static app_gui_screen_id_t s_current_screen_id = APP_GUI_SCREEN_NONE;
static portMUX_TYPE s_screen_id_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_latest_provisioning_status_available = false;
static ui_provisioning_status_t s_latest_provisioning_status = {
    .session_generation = 0U,
    .session_number = 0U,
    .session_limit = 0U,
    .state = UI_PROVISIONING_STATE_STARTING,
    .last_error = ESP_OK,
    .wifi_disconnect_reason = 0U,
};
static bool s_latest_provisioning_qr_payload_available = false;
static ui_provisioning_qr_payload_t
    s_latest_provisioning_qr_payload = {0};
static uint32_t s_latest_provisioning_generation = 0U;
static uint32_t s_newest_queued_provisioning_generation = 0U;
static bool s_latest_wifi_status_available = false;
static ui_wifi_status_t s_latest_wifi_status = {0};
static bool s_latest_sensor_status_available = false;
static ui_sensor_status_t s_latest_sensor_status = {0};
static bool s_latest_audio_status_available = false;
static ui_audio_status_t s_latest_audio_status = {
    .state = UI_AUDIO_STATE_UNAVAILABLE,
    .last_error = ESP_OK,
};
static bool s_latest_cloud_status_available = false;
static ui_cloud_status_t s_latest_cloud_status = {0};

/* Provisioning references are valid only while its screen is active. */
static lv_obj_t *s_provisioning_qr_container = NULL;
static lv_obj_t *s_provisioning_qr_code = NULL;
static lv_obj_t *s_provisioning_title_label = NULL;
static lv_obj_t *s_provisioning_instruction_label = NULL;
static lv_obj_t *s_provisioning_status_label = NULL;
static lv_obj_t *s_provisioning_state_indicator = NULL;

/* Wi-Fi object references are valid only while the Wi-Fi screen is active. */
static lv_obj_t *s_wifi_mode_label = NULL;
static lv_obj_t *s_wifi_ssid_label = NULL;
static lv_obj_t *s_wifi_ip_label = NULL;
static lv_timer_t *s_wifi_screen_timer = NULL;

/* Sensor object references are valid only while the sensor screen is active. */
static lv_obj_t *s_sensor_temperature_label = NULL;
static lv_obj_t *s_sensor_humidity_label = NULL;
static lv_obj_t *s_sensor_audio_label = NULL;
static lv_obj_t *s_sensor_state_label = NULL;
static lv_obj_t *s_sensor_wifi_label = NULL;
static lv_obj_t *s_sensor_wifi_dot = NULL;
static lv_obj_t *s_sensor_cloud_label = NULL;
static lv_obj_t *s_sensor_cloud_dot = NULL;

static bool s_latest_reset_status_available = false;

static ui_reset_status_t s_latest_reset_status =
{
    .transaction_id = 0U,
    .state = UI_RESET_STATE_FAILED,
    .last_error = ESP_FAIL,
};

/*
 * A reset result is acknowledged only after the UI task has processed the
 * command and completed the following LVGL timer/flush pass.
 */
static uint32_t s_pending_reset_presentation_id = 0U;
static uint32_t s_presented_reset_transaction_id = 0U;

/*
 * Once a reset-result screen is shown, normal screen-routing commands are
 * ignored. Another reset-result command remains allowed so a failed attempt
 * can later be replaced by a successful retry.
 */
static bool s_reset_result_route_locked = false;

/* Reset-result references are valid only while its screen is active. */
static lv_obj_t *s_reset_status_label = NULL;
static lv_obj_t *s_reset_detail_label = NULL;
static lv_obj_t *s_reset_state_indicator = NULL;

/* Function Prototypes ------------------------------------------------------ */
static bool app_gui_is_valid_screen_id(
    app_gui_screen_id_t screen_id,
    bool allow_none);
static void app_gui_zeroize(
    void *buffer,
    size_t size);
static bool app_gui_is_valid_provisioning_state(
    ui_provisioning_state_t state);
static bool app_gui_is_valid_provisioning_qr_payload(
    const ui_provisioning_qr_payload_t *payload);
static bool app_gui_accept_provisioning_generation(
    uint32_t session_generation,
    bool *is_newer_generation);
static bool app_gui_accept_queued_provisioning_generation(
    uint32_t session_generation);
static void app_gui_hide_provisioning_qr_if_active(void);
static const char *app_gui_screen_id_to_string(
    app_gui_screen_id_t screen_id);
static const char *app_gui_provisioning_state_to_string(
    ui_provisioning_state_t state);
static const char *app_gui_provisioning_instruction_text(
    ui_provisioning_state_t state);
static const char *app_gui_provisioning_status_text(
    ui_provisioning_state_t state);
static lv_color_t app_gui_provisioning_state_color(
    ui_provisioning_state_t state);
static void app_gui_cleanup_queues(void);
static void app_gui_set_active_screen_id(
    app_gui_screen_id_t screen_id);
static void app_gui_capture_widget_refs(
    app_gui_widget_refs_t *refs);
static void app_gui_clear_widget_refs(void);
static void app_gui_apply_widget_refs(
    const app_gui_widget_refs_t *refs);
static void app_gui_wifi_screen_timeout_cb(lv_timer_t *timer);
static void app_gui_restart_wifi_screen_timer(void);
static esp_err_t app_gui_create_boot_screen(
    lv_obj_t *screen);
static void app_gui_render_boot_status(
    lv_obj_t *screen);
static esp_err_t app_gui_create_provisioning_screen(
    lv_obj_t *screen);
static void app_gui_render_provisioning_qr_payload(
    const ui_provisioning_qr_payload_t *payload);
static void app_gui_render_provisioning_status(
    const ui_provisioning_status_t *status);
static esp_err_t app_gui_create_wifi_screen(
    lv_obj_t *screen);
static esp_err_t app_gui_create_sensor_screen(
    lv_obj_t *screen);
static const char *app_gui_wifi_state_to_string(ui_wifi_state_t state);
static lv_color_t app_gui_wifi_state_color(ui_wifi_state_t state);
static const char *app_gui_sensor_state_to_string(ui_sensor_state_t state);
static lv_color_t app_gui_sensor_state_color(ui_sensor_state_t state);
static const char *app_gui_audio_status_to_string(
    const ui_audio_status_t *status);
static lv_color_t app_gui_audio_status_color(
    const ui_audio_status_t *status);
static const char *app_gui_cloud_state_to_string(ui_cloud_state_t state);
static lv_color_t app_gui_cloud_state_color(ui_cloud_state_t state);
static lv_obj_t *app_gui_create_dashboard_rule(
    lv_obj_t *screen,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height);
static lv_obj_t *app_gui_create_dashboard_dot(
    lv_obj_t *screen,
    int32_t x,
    int32_t y,
    lv_color_t color);
static lv_obj_t *app_gui_create_wifi_value_label(
    lv_obj_t *screen,
    int32_t y,
    const char *initial_text);
static lv_obj_t *app_gui_create_sensor_value_label(
    lv_obj_t *screen,
    int32_t y,
    const char *initial_text);
static void app_gui_render_wifi_status(
    const ui_wifi_status_t *status);
static void app_gui_render_sensor_status(
    const ui_sensor_status_t *status);
static void app_gui_render_audio_status(
    const ui_audio_status_t *status);
static void app_gui_render_sensor_wifi_status(
    const ui_wifi_status_t *status);
static void app_gui_render_cloud_status(
    const ui_cloud_status_t *status);
static bool app_gui_render_cached_status(
    app_gui_screen_id_t screen_id);
static esp_err_t app_gui_activate_screen(
    app_gui_screen_id_t target_screen);
static void app_gui_process_provisioning_status(
    const ui_provisioning_status_t *status);
static bool app_gui_is_valid_reset_status(
    const ui_reset_status_t *status);

static const char *app_gui_reset_state_to_string(
    ui_reset_state_t state);

static esp_err_t app_gui_create_reset_result_screen(
    lv_obj_t *screen);

static bool app_gui_render_reset_status(
    const ui_reset_status_t *status);
static void app_gui_acknowledge_reset_presentation(void);
static void app_gui_process_provisioning_status_queue(void);
static void app_gui_process_commands(void);
static void app_gui_process_provisioning_qr_payload(void);
static void app_gui_process_sensor_status(void);
static void app_gui_process_audio_status(void);
static void app_gui_process_wifi_status(void);
static void app_gui_process_cloud_status(void);
static void app_gui_log_stack_usage(const char *task_name);
static void app_gui_process_lvgl(void);
static void app_gui_ui_task(void *param);

/* Static Functions --------------------------------------------------------- */
/* GUI Core Helpers --------------------------------------------------------- */
static bool app_gui_is_valid_screen_id(
    app_gui_screen_id_t screen_id,
    bool allow_none)
{
    return
        ((allow_none &&
          (screen_id == APP_GUI_SCREEN_NONE)) ||
         (screen_id == APP_GUI_SCREEN_BOOT) ||
         (screen_id == APP_GUI_SCREEN_PROVISIONING) ||
         (screen_id == APP_GUI_SCREEN_WIFI_STATUS) ||
         (screen_id == APP_GUI_SCREEN_SENSOR_DASHBOARD) ||
         (screen_id == APP_GUI_SCREEN_RESET_RESULT));
}

static bool app_gui_is_valid_reset_status(
    const ui_reset_status_t *status)
{
    if (status == NULL)
    {
        return false;
    }

    if (status->transaction_id == 0U)
    {
        return false;
    }

    switch (status->state)
    {
        case UI_RESET_STATE_SUCCESS:
            return status->last_error == ESP_OK;

        case UI_RESET_STATE_FAILED:
            return status->last_error != ESP_OK;

        default:
            return false;
    }
}

static const char *app_gui_reset_state_to_string(
    ui_reset_state_t state)
{
    switch (state)
    {
        case UI_RESET_STATE_SUCCESS:
            return "SUCCESS";

        case UI_RESET_STATE_FAILED:
            return "FAILED";

        default:
            return "UNKNOWN";
    }
}

static void app_gui_zeroize(
    void *buffer,
    size_t size)
{
    if ((buffer == NULL) ||
        (size == 0U)) {
        return;
    }

    volatile uint8_t *cursor =
        (volatile uint8_t *)buffer;

    while (size > 0U) {
        *cursor = 0U;
        cursor++;
        size--;
    }
}

static bool app_gui_is_valid_provisioning_state(
    ui_provisioning_state_t state)
{
    return
        (state >= UI_PROVISIONING_STATE_STARTING) &&
        (state <= UI_PROVISIONING_STATE_RETRYING);
}

static bool app_gui_is_valid_provisioning_qr_payload(
    const ui_provisioning_qr_payload_t *payload)
{
    if ((payload == NULL) ||
        (payload->session_generation == 0U) ||
        (payload->payload[0] == '\0'))
    {
        return false;
    }

    return
        strnlen(
            payload->payload,
            sizeof(payload->payload)) <
        sizeof(payload->payload);
}

static bool app_gui_accept_provisioning_generation(
    uint32_t session_generation,
    bool *is_newer_generation)
{
    if ((session_generation == 0U) ||
        (is_newer_generation == NULL))
    {
        return false;
    }

    *is_newer_generation = false;

    taskENTER_CRITICAL(&s_screen_id_lock);

    if ((s_latest_provisioning_generation != 0U) &&
        (session_generation <
         s_latest_provisioning_generation))
    {
        taskEXIT_CRITICAL(&s_screen_id_lock);

        return false;
    }

    if (session_generation >
        s_latest_provisioning_generation)
    {
        s_latest_provisioning_generation =
            session_generation;
        s_latest_provisioning_status_available = false;
        s_latest_provisioning_qr_payload_available = false;

        app_gui_zeroize(
            &s_latest_provisioning_status,
            sizeof(s_latest_provisioning_status));

        app_gui_zeroize(
            &s_latest_provisioning_qr_payload,
            sizeof(s_latest_provisioning_qr_payload));

        *is_newer_generation = true;
    }

    taskEXIT_CRITICAL(&s_screen_id_lock);

    return true;
}

static bool app_gui_accept_queued_provisioning_generation(
    uint32_t session_generation)
{
    if (session_generation == 0U)
    {
        return false;
    }

    taskENTER_CRITICAL(&s_screen_id_lock);

    if ((s_newest_queued_provisioning_generation != 0U) &&
        (session_generation <
         s_newest_queued_provisioning_generation))
    {
        taskEXIT_CRITICAL(&s_screen_id_lock);

        return false;
    }

    if (session_generation >
        s_newest_queued_provisioning_generation)
    {
        s_newest_queued_provisioning_generation =
            session_generation;
    }

    taskEXIT_CRITICAL(&s_screen_id_lock);

    return true;
}

static void app_gui_hide_provisioning_qr_if_active(void)
{
    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) != ESP_OK) ||
        (screen_id != APP_GUI_SCREEN_PROVISIONING))
    {
        return;
    }

    ui_manager_lvgl_wait_for_mutex();

    if (s_provisioning_qr_code != NULL)
    {
        lv_obj_add_flag(
            s_provisioning_qr_code,
            LV_OBJ_FLAG_HIDDEN);
    }

    ui_manager_lvgl_release_mutex();
}

static const char *app_gui_screen_id_to_string(
    app_gui_screen_id_t screen_id)
{
    switch (screen_id) {
        case APP_GUI_SCREEN_NONE:
            return "NONE";

        case APP_GUI_SCREEN_BOOT:
            return "BOOT";

        case APP_GUI_SCREEN_PROVISIONING:
            return "PROVISIONING";

        case APP_GUI_SCREEN_WIFI_STATUS:
            return "WIFI_STATUS";

        case APP_GUI_SCREEN_SENSOR_DASHBOARD:
            return "SENSOR_DASHBOARD";

        case APP_GUI_SCREEN_RESET_RESULT:
            return "RESET_RESULT";

        default:
            return "UNKNOWN";
    }
}

static const char *app_gui_provisioning_state_to_string(
    ui_provisioning_state_t state)
{
    switch (state) {
        case UI_PROVISIONING_STATE_STARTING:
            return "STARTING";

        case UI_PROVISIONING_STATE_WAITING_FOR_PHONE:
            return "WAITING_FOR_PHONE";

        case UI_PROVISIONING_STATE_CREDENTIAL_RECEIVED:
            return "CREDENTIAL_RECEIVED";

        case UI_PROVISIONING_STATE_CONNECTING_WIFI:
            return "CONNECTING_WIFI";

        case UI_PROVISIONING_STATE_WAITING_FOR_IP:
            return "WAITING_FOR_IP";

        case UI_PROVISIONING_STATE_SAVING_CONFIG:
            return "SAVING_CONFIG";

        case UI_PROVISIONING_STATE_CLEANING_UP:
            return "CLEANING_UP";

        case UI_PROVISIONING_STATE_SUCCESS:
            return "SUCCESS";

        case UI_PROVISIONING_STATE_FAILED:
            return "FAILED";

        case UI_PROVISIONING_STATE_TIMEOUT:
            return "TIMEOUT";

        case UI_PROVISIONING_STATE_RETRYING:
            return "RETRYING";

        default:
            return "UNKNOWN";
    }
}

static const char *app_gui_provisioning_instruction_text(
    ui_provisioning_state_t state)
{
    switch (state) {
        case UI_PROVISIONING_STATE_WAITING_FOR_PHONE:
            return "Scan to connect";

        case UI_PROVISIONING_STATE_CREDENTIAL_RECEIVED:
            return "Wi-Fi received";

        case UI_PROVISIONING_STATE_CONNECTING_WIFI:
            return "Connecting to Wi-Fi";

        case UI_PROVISIONING_STATE_WAITING_FOR_IP:
            return "Connected to router";

        case UI_PROVISIONING_STATE_SAVING_CONFIG:
            return "Saving settings";

        case UI_PROVISIONING_STATE_CLEANING_UP:
            return "Setup complete";

        case UI_PROVISIONING_STATE_SUCCESS:
            return "Wi-Fi configured";

        case UI_PROVISIONING_STATE_FAILED:
            return "Check Wi-Fi details";

        case UI_PROVISIONING_STATE_TIMEOUT:
            return "Setup expired";

        case UI_PROVISIONING_STATE_RETRYING:
            return "Starting a new session";

        case UI_PROVISIONING_STATE_STARTING:
        default:
            return "Prepare your phone";
    }
}

static const char *app_gui_provisioning_status_text(
    ui_provisioning_state_t state)
{
    switch (state) {
        case UI_PROVISIONING_STATE_WAITING_FOR_PHONE:
            return "Waiting for phone";

        case UI_PROVISIONING_STATE_CREDENTIAL_RECEIVED:
            return "Checking...";

        case UI_PROVISIONING_STATE_CONNECTING_WIFI:
            return "Connecting...";

        case UI_PROVISIONING_STATE_WAITING_FOR_IP:
            return "Getting IP...";

        case UI_PROVISIONING_STATE_SAVING_CONFIG:
            return "Saving...";

        case UI_PROVISIONING_STATE_CLEANING_UP:
            return "Finishing...";

        case UI_PROVISIONING_STATE_SUCCESS:
            return "Connected";

        case UI_PROVISIONING_STATE_FAILED:
            return "Connection failed";

        case UI_PROVISIONING_STATE_TIMEOUT:
            return "Timed out";

        case UI_PROVISIONING_STATE_RETRYING:
            return "Retrying...";

        case UI_PROVISIONING_STATE_STARTING:
        default:
            return "Starting setup...";
    }
}

static lv_color_t app_gui_provisioning_state_color(
    ui_provisioning_state_t state)
{
    switch (state) {
        case UI_PROVISIONING_STATE_SUCCESS:
            return lv_color_hex(0x49C978);

        case UI_PROVISIONING_STATE_FAILED:
        case UI_PROVISIONING_STATE_TIMEOUT:
            return lv_color_hex(0xF06464);

        case UI_PROVISIONING_STATE_SAVING_CONFIG:
        case UI_PROVISIONING_STATE_CLEANING_UP:
        case UI_PROVISIONING_STATE_RETRYING:
            return lv_color_hex(0xFFC857);

        case UI_PROVISIONING_STATE_STARTING:
        case UI_PROVISIONING_STATE_WAITING_FOR_PHONE:
        case UI_PROVISIONING_STATE_CREDENTIAL_RECEIVED:
        case UI_PROVISIONING_STATE_CONNECTING_WIFI:
        case UI_PROVISIONING_STATE_WAITING_FOR_IP:
        default:
            return lv_color_hex(0x4DB6E5);
    }
}

static const char *app_gui_audio_status_to_string(
    const ui_audio_status_t *status)
{
    if (status == NULL)
    {
        return "Audio: --";
    }

    if ((status->state == UI_AUDIO_STATE_IDLE) &&
        (status->last_error != ESP_OK))
    {
        return "Audio: ERR";
    }

    switch (status->state)
    {
        case UI_AUDIO_STATE_READY:
            return "Audio: Ready";

        case UI_AUDIO_STATE_IDLE:
            return "Audio: Idle";

        case UI_AUDIO_STATE_RECORDING:
            return "Audio: REC";

        case UI_AUDIO_STATE_PROCESSING:
            return "Audio: DSP";

        case UI_AUDIO_STATE_PLAYBACK:
            return "Audio: PLAY";

        case UI_AUDIO_STATE_ERROR:
            return "Audio: ERR";

        case UI_AUDIO_STATE_UNAVAILABLE:
        default:
            return "Audio: --";
    }
}

static lv_color_t app_gui_audio_status_color(
    const ui_audio_status_t *status)
{
    if ((status == NULL) ||
        (status->state == UI_AUDIO_STATE_UNAVAILABLE))
    {
        return lv_color_hex(0x7B858A);
    }

    if ((status->state == UI_AUDIO_STATE_IDLE) &&
        (status->last_error != ESP_OK))
    {
        return lv_color_hex(0xF06464);
    }

    switch (status->state)
    {
        case UI_AUDIO_STATE_RECORDING:
            return lv_color_hex(0xF06464);

        case UI_AUDIO_STATE_PROCESSING:
            return lv_color_hex(0xFFC857);

        case UI_AUDIO_STATE_PLAYBACK:
            return lv_color_hex(0x49C978);

        case UI_AUDIO_STATE_ERROR:
            return lv_color_hex(0xF06464);

        case UI_AUDIO_STATE_READY:
            return lv_color_hex(0x4DB6E5);

        case UI_AUDIO_STATE_IDLE:
        default:
            return lv_color_hex(0x7B858A);
    }
}

static void app_gui_cleanup_queues(void)
{
    if (s_command_queue != NULL) {
        vQueueDeleteWithCaps(s_command_queue);
        s_command_queue = NULL;
    }

    if (s_provisioning_status_queue != NULL) {
        vQueueDeleteWithCaps(s_provisioning_status_queue);
        s_provisioning_status_queue = NULL;
    }

    if (s_provisioning_qr_queue != NULL) {
        vQueueDeleteWithCaps(s_provisioning_qr_queue);
        s_provisioning_qr_queue = NULL;
    }

    if (s_wifi_status_queue != NULL) {
        vQueueDeleteWithCaps(s_wifi_status_queue);
        s_wifi_status_queue = NULL;
    }

    if (s_sensor_status_queue != NULL) {
        vQueueDeleteWithCaps(s_sensor_status_queue);
        s_sensor_status_queue = NULL;
    }

    if (s_audio_status_queue != NULL) {
        vQueueDeleteWithCaps(s_audio_status_queue);
        s_audio_status_queue = NULL;
    }

    if (s_cloud_status_queue != NULL) {
        vQueueDeleteWithCaps(s_cloud_status_queue);
        s_cloud_status_queue = NULL;
    }
}

static void app_gui_set_active_screen_id(
    app_gui_screen_id_t screen_id)
{
    taskENTER_CRITICAL(&s_screen_id_lock);
    s_current_screen_id = screen_id;
    taskEXIT_CRITICAL(&s_screen_id_lock);
}

static void app_gui_capture_widget_refs(
    app_gui_widget_refs_t *refs)
{
    if (refs == NULL) {
        return;
    }

    refs->provisioning_qr_container =
        s_provisioning_qr_container;
    refs->provisioning_qr_code =
        s_provisioning_qr_code;
    refs->provisioning_title_label = s_provisioning_title_label;
    refs->provisioning_instruction_label =
        s_provisioning_instruction_label;
    refs->provisioning_status_label = s_provisioning_status_label;
    refs->provisioning_state_indicator =
        s_provisioning_state_indicator;
    refs->wifi_mode_label = s_wifi_mode_label;
    refs->wifi_ssid_label = s_wifi_ssid_label;
    refs->wifi_ip_label = s_wifi_ip_label;
    refs->sensor_temperature_label = s_sensor_temperature_label;
    refs->sensor_humidity_label = s_sensor_humidity_label;
    refs->sensor_audio_label = s_sensor_audio_label;
    refs->sensor_state_label = s_sensor_state_label;
    refs->sensor_wifi_label = s_sensor_wifi_label;
    refs->sensor_wifi_dot = s_sensor_wifi_dot;
    refs->sensor_cloud_label = s_sensor_cloud_label;
    refs->sensor_cloud_dot = s_sensor_cloud_dot;
    refs->reset_status_label =
        s_reset_status_label;

    refs->reset_detail_label =
        s_reset_detail_label;

    refs->reset_state_indicator =
        s_reset_state_indicator;
}

static void app_gui_clear_widget_refs(void)
{
    s_provisioning_qr_container = NULL;
    s_provisioning_qr_code = NULL;
    s_provisioning_title_label = NULL;
    s_provisioning_instruction_label = NULL;
    s_provisioning_status_label = NULL;
    s_provisioning_state_indicator = NULL;
    s_wifi_mode_label = NULL;
    s_wifi_ssid_label = NULL;
    s_wifi_ip_label = NULL;
    s_sensor_temperature_label = NULL;
    s_sensor_humidity_label = NULL;
    s_sensor_audio_label = NULL;
    s_sensor_state_label = NULL;
    s_sensor_wifi_label = NULL;
    s_sensor_wifi_dot = NULL;
    s_sensor_cloud_label = NULL;
    s_sensor_cloud_dot = NULL;
    s_reset_status_label = NULL;
    s_reset_detail_label = NULL;
    s_reset_state_indicator = NULL;
}

static void app_gui_apply_widget_refs(
    const app_gui_widget_refs_t *refs)
{
    app_gui_clear_widget_refs();

    if (refs == NULL) {
        return;
    }

    s_provisioning_qr_container =
        refs->provisioning_qr_container;
    s_provisioning_qr_code =
        refs->provisioning_qr_code;
    s_provisioning_title_label = refs->provisioning_title_label;
    s_provisioning_instruction_label =
        refs->provisioning_instruction_label;
    s_provisioning_status_label = refs->provisioning_status_label;
    s_provisioning_state_indicator =
        refs->provisioning_state_indicator;
    s_wifi_mode_label = refs->wifi_mode_label;
    s_wifi_ssid_label = refs->wifi_ssid_label;
    s_wifi_ip_label = refs->wifi_ip_label;
    s_sensor_temperature_label = refs->sensor_temperature_label;
    s_sensor_humidity_label = refs->sensor_humidity_label;
    s_sensor_audio_label = refs->sensor_audio_label;
    s_sensor_state_label = refs->sensor_state_label;
    s_sensor_wifi_label = refs->sensor_wifi_label;
    s_sensor_wifi_dot = refs->sensor_wifi_dot;
    s_sensor_cloud_label = refs->sensor_cloud_label;
    s_sensor_cloud_dot = refs->sensor_cloud_dot;
    s_reset_status_label = refs->reset_status_label;
    s_reset_detail_label = refs->reset_detail_label;
    s_reset_state_indicator = refs->reset_state_indicator;
}

/* Wi-Fi Screen Helpers ----------------------------------------------------- */
static void app_gui_wifi_screen_timeout_cb(lv_timer_t *timer)
{
    /*
     * LVGL invokes this callback while the GUI task owns the non-recursive
     * LVGL mutex. Defer construction to the next GUI iteration so the current
     * Wi-Fi screen remains visible until its replacement is complete.
     */
    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) != ESP_OK) ||
        (screen_id != APP_GUI_SCREEN_WIFI_STATUS)) {
        lv_timer_pause(timer);
        return;
    }

    const esp_err_t ret =
        app_gui_request_screen(
            APP_GUI_SCREEN_SENSOR_DASHBOARD);

    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Wi-Fi timeout screen request failed: %s",
            esp_err_to_name(ret));
    }
    else {
        ESP_LOGD(
            TAG,
            "Wi-Fi screen timeout; sensor dashboard queued");
    }

    /* Make this timer behave like a reusable one-shot timer. */
    lv_timer_pause(timer);
}

static void app_gui_restart_wifi_screen_timer(void)
{
    if (s_wifi_screen_timer == NULL) {
        return;
    }

    lv_timer_set_period(
        s_wifi_screen_timer,
        APP_GUI_WIFI_SCREEN_TIMEOUT_MS);

    lv_timer_resume(s_wifi_screen_timer);
    lv_timer_reset(s_wifi_screen_timer);
    ESP_LOGD(TAG, "Timer restarted");
}

static const char *app_gui_wifi_state_to_string(ui_wifi_state_t state)
{
    switch (state) {
        case UI_WIFI_STATE_CONNECTING:
            return "CONNECTING";

        case UI_WIFI_STATE_WAITING_FOR_IP:
            return "WAITING IP";

        case UI_WIFI_STATE_CONNECTED:
            return "CONNECTED";

        case UI_WIFI_STATE_DISCONNECTED:
            return "DISCONNECTED";

        case UI_WIFI_STATE_FAILED:
            return "FAILED";

        case UI_WIFI_STATE_RETRY_WAIT:
            return "RECONNECTING";

        case UI_WIFI_STATE_IDLE:
        default:
            return "IDLE";
    }
}

static lv_color_t app_gui_wifi_state_color(ui_wifi_state_t state)
{
    switch (state) {
        case UI_WIFI_STATE_CONNECTING:
            return lv_color_hex(0xFFC857);

        case UI_WIFI_STATE_WAITING_FOR_IP:
            return lv_color_hex(0x4DB6E5);

        case UI_WIFI_STATE_CONNECTED:
            return lv_color_hex(0x49C978);

        case UI_WIFI_STATE_DISCONNECTED:
        case UI_WIFI_STATE_FAILED:
            return lv_color_hex(0xF06464);

        case UI_WIFI_STATE_IDLE:
        default:
            return lv_color_hex(0xA6B0B6);
    }
}

/* Sensor Screen Helpers --------------------------------------------------- */
static const char *app_gui_sensor_state_to_string(
    ui_sensor_state_t state)
{
    switch (state) {
        case UI_SENSOR_STATE_READY:
            return "Sensor: OK";

        case UI_SENSOR_STATE_DEGRADED:
            return "Sensor: Warn";

        case UI_SENSOR_STATE_ERROR:
            return "Sensor: Error";

        case UI_SENSOR_STATE_INITIALIZING:
        default:
            return "Sensor: --";
    }
}

static lv_color_t app_gui_sensor_state_color(
    ui_sensor_state_t state)
{
    switch (state) {
        case UI_SENSOR_STATE_READY:
            return lv_color_hex(0x49C978);

        case UI_SENSOR_STATE_DEGRADED:
            return lv_color_hex(0xFFC857);

        case UI_SENSOR_STATE_ERROR:
            return lv_color_hex(0xF06464);

        case UI_SENSOR_STATE_INITIALIZING:
        default:
            return lv_color_hex(0x4DB6E5);
    }
}

static const char *app_gui_cloud_state_to_string(
    ui_cloud_state_t state)
{
    switch (state) {
        case UI_CLOUD_STATE_ONLINE:
            return "Cloud: Online";

        case UI_CLOUD_STATE_UPLOADING:
            return "Cloud: Sync";

        case UI_CLOUD_STATE_WAITING:
            return "Cloud: Wait";

        case UI_CLOUD_STATE_RETRY_WAIT:
            return "Cloud: Retry";

        case UI_CLOUD_STATE_AUTH_ERROR:
            return "Cloud: Auth";

        case UI_CLOUD_STATE_ERROR:
            return "Cloud: Error";

        case UI_CLOUD_STATE_UNKNOWN:
        default:
            return "Cloud: --";
    }
}

static lv_color_t app_gui_cloud_state_color(
    ui_cloud_state_t state)
{
    switch (state) {
        case UI_CLOUD_STATE_ONLINE:
            return lv_color_hex(0x49C978);

        case UI_CLOUD_STATE_UPLOADING:
            return lv_color_hex(0x4DB6E5);

        case UI_CLOUD_STATE_WAITING:
        case UI_CLOUD_STATE_RETRY_WAIT:
            return lv_color_hex(0xFFC857);

        case UI_CLOUD_STATE_AUTH_ERROR:
        case UI_CLOUD_STATE_ERROR:
            return lv_color_hex(0xF06464);

        case UI_CLOUD_STATE_UNKNOWN:
        default:
            return lv_color_hex(0x7B858A);
    }
}

static lv_obj_t *app_gui_create_dashboard_rule(
    lv_obj_t *screen,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
    lv_obj_t *rule = lv_obj_create(screen);
    if (rule == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, width, height);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        rule,
        lv_color_hex(0xDDE4E6),
        LV_PART_MAIN);

    return rule;
}

static lv_obj_t *app_gui_create_dashboard_dot(
    lv_obj_t *screen,
    int32_t x,
    int32_t y,
    lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(screen);
    if (dot == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);

    return dot;
}

static lv_obj_t *app_gui_create_wifi_value_label(
    lv_obj_t *screen,
    int32_t y,
    const char *initial_text)
{
    lv_obj_t *label = lv_label_create(screen);
    if (label == NULL) {
        return NULL;
    }

    lv_label_set_text(label, initial_text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(label,
                    APP_GUI_WIFI_VALUE_WIDTH_PX,
                    APP_GUI_WIFI_VALUE_HEIGHT_PX);
    lv_obj_set_pos(label, 52, y);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(0xF2F5F7),
                                LV_PART_MAIN);

    lv_obj_set_style_text_font(label,
                    &lv_font_montserrat_12,
                    LV_PART_MAIN);
    lv_obj_set_style_text_align(label,
                                LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);

    return label;
}

static lv_obj_t *app_gui_create_sensor_value_label(
    lv_obj_t *screen,
    int32_t y,
    const char *initial_text)
{
    lv_obj_t *label = lv_label_create(screen);
    if (label == NULL) {
        return NULL;
    }

    lv_label_set_text(label, initial_text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(label,
                    APP_GUI_SENSOR_VALUE_WIDTH_PX,
                    APP_GUI_SENSOR_VALUE_HEIGHT_PX);
    lv_obj_set_pos(label, 60, y);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(0xF2F5F7),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(label,
                               &lv_font_montserrat_12,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(label,
                                LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);

    return label;
}

/* Placeholder Screen Construction ----------------------------------------- */
static esp_err_t app_gui_create_boot_screen(
    lv_obj_t *screen)
{
    if (screen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(0x101619),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_t *status = lv_label_create(screen);

    if ((title == NULL) || (status == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(title, "Smart Gateway");
    lv_obj_set_style_text_font(
        title,
        &lv_font_montserrat_20,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        title,
        lv_color_hex(0xF2F5F7),
        LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -14);

    app_gui_render_boot_status(status);

    return ESP_OK;
}

static void app_gui_render_boot_status(
    lv_obj_t *status_label)
{
    if (status_label == NULL) {
        return;
    }

    lv_label_set_text(status_label, "Starting...");
    lv_obj_set_style_text_font(
        status_label,
        &lv_font_montserrat_12,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        status_label,
        lv_color_hex(0x8C989F),
        LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 14);
}

static esp_err_t app_gui_create_provisioning_screen(
    lv_obj_t *screen)
{
    if (screen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(0x101619),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    s_provisioning_title_label =
        lv_label_create(screen);

    if (s_provisioning_title_label == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning title");
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(
        s_provisioning_title_label,
        "Wi-Fi Setup");
    lv_obj_set_pos(s_provisioning_title_label, 6, 5);
    lv_obj_set_style_text_font(
        s_provisioning_title_label,
        &lv_font_montserrat_18,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        s_provisioning_title_label,
        lv_color_hex(0xF2F5F7),
        LV_PART_MAIN);

    s_provisioning_state_indicator =
        lv_obj_create(screen);

    if (s_provisioning_state_indicator == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning state indicator");
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(
        s_provisioning_state_indicator);
    lv_obj_set_size(
        s_provisioning_state_indicator,
        APP_GUI_PROVISIONING_INDICATOR_SIZE_PX,
        APP_GUI_PROVISIONING_INDICATOR_SIZE_PX);
    lv_obj_set_pos(
        s_provisioning_state_indicator,
        146,
        10);
    lv_obj_set_style_radius(
        s_provisioning_state_indicator,
        LV_RADIUS_CIRCLE,
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        s_provisioning_state_indicator,
        LV_OPA_COVER,
        LV_PART_MAIN);

    lv_obj_t *divider = lv_obj_create(screen);

    if (divider == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning divider");
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 148, 1);
    lv_obj_set_pos(divider, 6, 31);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        divider,
        lv_color_hex(0x344047),
        LV_PART_MAIN);

    s_provisioning_qr_container =
        lv_obj_create(screen);

    if (s_provisioning_qr_container == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning QR container");
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(
        s_provisioning_qr_container);
    lv_obj_remove_flag(
        s_provisioning_qr_container,
        LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(
        s_provisioning_qr_container,
        APP_GUI_PROVISIONING_QR_CONTAINER_SIZE_PX,
        APP_GUI_PROVISIONING_QR_CONTAINER_SIZE_PX);
    lv_obj_set_pos(
        s_provisioning_qr_container,
        APP_GUI_PROVISIONING_QR_CONTAINER_X_PX,
        APP_GUI_PROVISIONING_QR_CONTAINER_Y_PX);
    lv_obj_set_style_bg_color(
        s_provisioning_qr_container,
        lv_color_white(),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        s_provisioning_qr_container,
        LV_OPA_COVER,
        LV_PART_MAIN);
    lv_obj_set_style_border_width(
        s_provisioning_qr_container,
        0,
        LV_PART_MAIN);
    lv_obj_set_style_radius(
        s_provisioning_qr_container,
        0,
        LV_PART_MAIN);
    lv_obj_set_style_pad_all(
        s_provisioning_qr_container,
        0,
        LV_PART_MAIN);

    s_provisioning_qr_code =
        lv_qrcode_create(
            s_provisioning_qr_container);

    if (s_provisioning_qr_code == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning QR code");
        return ESP_ERR_NO_MEM;
    }

    lv_qrcode_set_size(
        s_provisioning_qr_code,
        APP_GUI_PROVISIONING_QR_CANVAS_SIZE_PX);
    lv_qrcode_set_dark_color(
        s_provisioning_qr_code,
        lv_color_black());
    lv_qrcode_set_light_color(
        s_provisioning_qr_code,
        lv_color_white());

    /*
     * The white parent supplies a measured quiet zone. Keeping it outside the
     * encoded canvas preserves exact 2x2 square modules.
     */
    lv_qrcode_set_quiet_zone(
        s_provisioning_qr_code,
        false);
    lv_obj_center(
        s_provisioning_qr_code);
    lv_obj_update_layout(
        s_provisioning_qr_code);

    if ((lv_obj_get_width(s_provisioning_qr_code) !=
         APP_GUI_PROVISIONING_QR_CANVAS_SIZE_PX) ||
        (lv_obj_get_height(s_provisioning_qr_code) !=
         APP_GUI_PROVISIONING_QR_CANVAS_SIZE_PX)) {
        ESP_LOGE(TAG, "Failed to allocate provisioning QR canvas");
        return ESP_ERR_NO_MEM;
    }

    lv_obj_add_flag(
        s_provisioning_qr_code,
        LV_OBJ_FLAG_HIDDEN);

    s_provisioning_instruction_label =
        lv_label_create(screen);

    if (s_provisioning_instruction_label == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning instruction");
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(
        s_provisioning_instruction_label,
        "");
    lv_label_set_long_mode(
        s_provisioning_instruction_label,
        LV_LABEL_LONG_WRAP);
    lv_obj_set_size(
        s_provisioning_instruction_label,
        APP_GUI_PROVISIONING_TEXT_WIDTH_PX,
        44);
    lv_obj_set_pos(
        s_provisioning_instruction_label,
        APP_GUI_PROVISIONING_TEXT_X_PX,
        41);
    lv_obj_set_style_text_font(
        s_provisioning_instruction_label,
        &lv_font_montserrat_12,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        s_provisioning_instruction_label,
        lv_color_hex(0xF2F5F7),
        LV_PART_MAIN);
    lv_obj_set_style_text_align(
        s_provisioning_instruction_label,
        LV_TEXT_ALIGN_CENTER,
        LV_PART_MAIN);

    s_provisioning_status_label =
        lv_label_create(screen);

    if (s_provisioning_status_label == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning status");
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(
        s_provisioning_status_label,
        "");
    lv_label_set_long_mode(
        s_provisioning_status_label,
        LV_LABEL_LONG_WRAP);
    lv_obj_set_size(
        s_provisioning_status_label,
        APP_GUI_PROVISIONING_TEXT_WIDTH_PX,
        31);
    lv_obj_set_pos(
        s_provisioning_status_label,
        APP_GUI_PROVISIONING_TEXT_X_PX,
        89);
    lv_obj_set_style_text_font(
        s_provisioning_status_label,
        &lv_font_montserrat_10,
        LV_PART_MAIN);
    lv_obj_set_style_text_align(
        s_provisioning_status_label,
        LV_TEXT_ALIGN_CENTER,
        LV_PART_MAIN);

    return ESP_OK;
}

static void app_gui_render_provisioning_qr_payload(
    const ui_provisioning_qr_payload_t *payload)
{
    if (!app_gui_is_valid_provisioning_qr_payload(payload) ||
        (s_provisioning_qr_container == NULL) ||
        (s_provisioning_qr_code == NULL)) {
        return;
    }

    const size_t payload_length =
        strnlen(
            payload->payload,
            sizeof(payload->payload));

    const lv_result_t result =
        lv_qrcode_update(
            s_provisioning_qr_code,
            payload->payload,
            (uint32_t)payload_length);

    if (result != LV_RESULT_OK) {
        lv_obj_add_flag(
            s_provisioning_qr_code,
            LV_OBJ_FLAG_HIDDEN);
        ESP_LOGE(TAG, "Failed to encode provisioning QR payload");
        return;
    }

    lv_obj_remove_flag(
        s_provisioning_qr_code,
        LV_OBJ_FLAG_HIDDEN);
}

static void app_gui_render_provisioning_status(
    const ui_provisioning_status_t *status)
{
    if ((status == NULL) ||
        !app_gui_is_valid_provisioning_state(status->state) ||
        (s_provisioning_instruction_label == NULL) ||
        (s_provisioning_status_label == NULL) ||
        (s_provisioning_state_indicator == NULL)) {
        return;
    }

    const lv_color_t state_color =
        app_gui_provisioning_state_color(status->state);

    const char *instruction =
        app_gui_provisioning_instruction_text(
            status->state);

    char instruction_with_session
        [APP_GUI_PROVISIONING_INSTRUCTION_BUFFER_SIZE] =
        {0};

    if ((status->session_number > 0U) &&
        (status->session_limit > 0U) &&
        (status->session_number <=
         status->session_limit))
    {
        const int written =
            snprintf(
                instruction_with_session,
                sizeof(instruction_with_session),
                "Session %lu/%lu\n%s",
                (unsigned long)status->session_number,
                (unsigned long)status->session_limit,
                instruction);

        if ((written > 0) &&
            ((size_t)written <
             sizeof(instruction_with_session)))
        {
            instruction =
                instruction_with_session;
        }
    }

    lv_label_set_text(
        s_provisioning_instruction_label,
        instruction);
    lv_label_set_text(
        s_provisioning_status_label,
        app_gui_provisioning_status_text(status->state));
    lv_obj_set_style_text_color(
        s_provisioning_status_label,
        state_color,
        LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        s_provisioning_state_indicator,
        state_color,
        LV_PART_MAIN);
}

/* Wi-Fi Screen Construction ----------------------------------------------- */
static esp_err_t app_gui_create_wifi_screen(
    lv_obj_t *screen)
{
    if (screen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_gui_clear_widget_refs();

    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,
                              lv_color_hex(0x101619),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    if (title == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(title, "WI-FI STATUS");
    lv_obj_set_pos(title, 8, 4);
    lv_obj_set_style_text_font(title,
                               &lv_font_montserrat_20,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title,
                                lv_color_hex(0xF2F5F7),
                                LV_PART_MAIN);

    lv_obj_t *divider = lv_obj_create(screen);
    if (divider == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 144, 1);
    lv_obj_set_pos(divider, 8, 30);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(divider,
                              lv_color_hex(0x344047),
                              LV_PART_MAIN);

    static const struct {
        const char *text;
        int32_t y;
    } field_labels[] = {
        {"MODE", 39},
        {"SSID", 66},
        {"IP",   93},
    };

    for (size_t index = 0U;
         index < (sizeof(field_labels) / sizeof(field_labels[0]));
         ++index) {
        lv_obj_t *field = lv_label_create(screen);
        if (field == NULL) {
            return ESP_ERR_NO_MEM;
        }

        lv_label_set_text(field, field_labels[index].text);
        lv_obj_set_pos(field, 8, field_labels[index].y);
        lv_obj_set_style_text_font(field,
                                   &lv_font_montserrat_12,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(field,
                                    lv_color_hex(0x8C989F),
                                    LV_PART_MAIN);
    }

    s_wifi_mode_label =
        app_gui_create_wifi_value_label(screen, 39, "IDLE");
    s_wifi_ssid_label =
        app_gui_create_wifi_value_label(screen, 66, "-");
    s_wifi_ip_label =
        app_gui_create_wifi_value_label(screen, 93, "-");

    if ((s_wifi_mode_label == NULL) ||
        (s_wifi_ssid_label == NULL) ||
        (s_wifi_ip_label == NULL)) {
        s_wifi_mode_label = NULL;
        s_wifi_ssid_label = NULL;
        s_wifi_ip_label = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Give the longest IPv4 string more horizontal room than other values. */
    lv_obj_set_pos(s_wifi_ip_label, 32, 93);
    lv_obj_set_width(s_wifi_ip_label, APP_GUI_WIFI_IP_VALUE_WIDTH_PX);

    lv_obj_set_style_text_color(
        s_wifi_mode_label,
        app_gui_wifi_state_color(UI_WIFI_STATE_IDLE),
        LV_PART_MAIN);

    return ESP_OK;
}

/* Sensor Screen Construction ---------------------------------------------- */
static esp_err_t app_gui_create_sensor_screen(
    lv_obj_t *screen)
{
    if (screen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_gui_clear_widget_refs();

    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,
                              lv_color_hex(0x202223),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        screen,
        lv_color_hex(0xDDE4E6),
        LV_PART_MAIN);
    lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    bool wifi_status_available = false;
    bool wifi_online = false;
    bool cloud_status_available = false;
    ui_cloud_state_t cloud_state = UI_CLOUD_STATE_UNKNOWN;

    taskENTER_CRITICAL(&s_screen_id_lock);
    wifi_status_available = s_latest_wifi_status_available;
    wifi_online =
        s_latest_wifi_status.state == UI_WIFI_STATE_CONNECTED &&
        s_latest_wifi_status.has_ipv4_address;
    cloud_status_available = s_latest_cloud_status_available;
    cloud_state = s_latest_cloud_status.state;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    const lv_color_t inactive_color = lv_color_hex(0x7B858A);
    const lv_color_t wifi_color =
        !wifi_status_available
            ? inactive_color
            : (wifi_online
                ? lv_color_hex(0x49C978)
                : lv_color_hex(0xF06464));
    const lv_color_t cloud_color =
        cloud_status_available
            ? app_gui_cloud_state_color(cloud_state)
            : inactive_color;

    lv_obj_t *title = lv_label_create(screen);
    if (title == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(title, "Smart Room");
    lv_obj_set_pos(title, 7, 7);
    lv_obj_set_style_text_font(title,
                               &lv_font_montserrat_10,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(title,
                               lv_color_hex(0xF2F5F7),
                               LV_PART_MAIN);

    lv_obj_t *wifi_header = lv_label_create(screen);
    lv_obj_t *cloud_header = lv_label_create(screen);
    lv_obj_t *header_rule = app_gui_create_dashboard_rule(
        screen,
        1,
        APP_GUI_DASHBOARD_HEADER_HEIGHT_PX - 1,
        APP_GUI_DASHBOARD_WIDTH_PX - 2,
        1);
    lv_obj_t *column_rule = app_gui_create_dashboard_rule(
        screen,
        APP_GUI_DASHBOARD_COLUMN_X_PX,
        APP_GUI_DASHBOARD_HEADER_HEIGHT_PX,
        1,
        APP_GUI_DASHBOARD_STATUS_PANEL_HEIGHT_PX);
    s_sensor_wifi_dot = app_gui_create_dashboard_dot(
        screen,
        128,
        10,
        wifi_color);
    s_sensor_cloud_dot = app_gui_create_dashboard_dot(
        screen,
        151,
        10,
        cloud_color);

    if ((wifi_header == NULL) ||
        (cloud_header == NULL) ||
        (header_rule == NULL) ||
        (column_rule == NULL) ||
        (s_sensor_wifi_dot == NULL) ||
        (s_sensor_cloud_dot == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(wifi_header, "WiFi");
    lv_obj_set_pos(wifi_header, 102, 7);
    lv_obj_set_style_text_font(
        wifi_header,
        &lv_font_montserrat_10,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        wifi_header,
        lv_color_hex(0xF2F5F7),
        LV_PART_MAIN);

    lv_label_set_text(cloud_header, "C");
    lv_obj_set_pos(cloud_header, 141, 7);
    lv_obj_set_style_text_font(
        cloud_header,
        &lv_font_montserrat_10,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        cloud_header,
        lv_color_hex(0xF2F5F7),
        LV_PART_MAIN);

    s_sensor_temperature_label =
        app_gui_create_sensor_value_label(screen, 47, "-");
    s_sensor_humidity_label =
        app_gui_create_sensor_value_label(screen, 91, "-");
    s_sensor_audio_label =
        app_gui_create_sensor_value_label(
            screen,
            APP_GUI_DASHBOARD_STATUS_AUDIO_Y_PX,
            "Audio: --");
    s_sensor_wifi_label =
        app_gui_create_sensor_value_label(
            screen,
            APP_GUI_DASHBOARD_STATUS_WIFI_Y_PX,
            !wifi_status_available
                ? "Wi-Fi: --"
                : (wifi_online
                    ? "Wi-Fi: Online"
                    : "Wi-Fi: Offline"));
    s_sensor_cloud_label =
        app_gui_create_sensor_value_label(
            screen,
            APP_GUI_DASHBOARD_STATUS_CLOUD_Y_PX,
            cloud_status_available
                ? app_gui_cloud_state_to_string(cloud_state)
                : "Cloud: --");
    s_sensor_state_label =
        app_gui_create_sensor_value_label(
            screen,
            APP_GUI_DASHBOARD_STATUS_SENSOR_Y_PX,
            "Sensor: --");

    if ((s_sensor_temperature_label == NULL) ||
        (s_sensor_humidity_label == NULL) ||
        (s_sensor_audio_label == NULL) ||
        (s_sensor_wifi_label == NULL) ||
        (s_sensor_cloud_label == NULL) ||
        (s_sensor_state_label == NULL)) {
        app_gui_clear_widget_refs();
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *left_values[] = {
        s_sensor_temperature_label,
        s_sensor_humidity_label,
    };

    for (size_t index = 0U;
         index < (sizeof(left_values) / sizeof(left_values[0]));
         ++index) {
        lv_obj_set_x(
            left_values[index],
            APP_GUI_DASHBOARD_MARGIN_PX);
        lv_obj_set_width(
            left_values[index],
            APP_GUI_DASHBOARD_COLUMN_X_PX -
                (2 * APP_GUI_DASHBOARD_MARGIN_PX));
        lv_obj_set_style_text_align(
            left_values[index],
            LV_TEXT_ALIGN_CENTER,
            LV_PART_MAIN);
    }

    lv_obj_t *right_values[] = {
        s_sensor_wifi_label,
        s_sensor_cloud_label,
        s_sensor_state_label,
        s_sensor_audio_label,
    };

    for (size_t index = 0U;
         index < (sizeof(right_values) / sizeof(right_values[0]));
         ++index) {
        lv_obj_set_x(right_values[index], APP_GUI_DASHBOARD_RIGHT_X_PX);
        lv_obj_set_width(
            right_values[index],
            APP_GUI_DASHBOARD_RIGHT_WIDTH_PX);
        lv_obj_set_height(
            right_values[index],
            APP_GUI_DASHBOARD_STATUS_LABEL_HEIGHT_PX);
        lv_obj_set_style_pad_ver(
            right_values[index],
            0,
            LV_PART_MAIN);
        lv_obj_set_style_text_font(
            right_values[index],
            &lv_font_montserrat_10,
            LV_PART_MAIN);
        lv_obj_set_style_text_align(
            right_values[index],
            LV_TEXT_ALIGN_LEFT,
            LV_PART_MAIN);
    }

    lv_obj_set_style_text_color(
        s_sensor_wifi_label,
        wifi_color,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        s_sensor_cloud_label,
        cloud_color,
        LV_PART_MAIN);

    lv_obj_set_style_text_color(
        s_sensor_state_label,
        app_gui_sensor_state_color(UI_SENSOR_STATE_INITIALIZING),
        LV_PART_MAIN);

    lv_obj_set_style_text_color(
        s_sensor_audio_label,
        inactive_color,
        LV_PART_MAIN);

    return ESP_OK;
}

/* Reset Result Screen ----------------------------------------------------- */

static esp_err_t app_gui_create_reset_result_screen(
    lv_obj_t *screen)
{
    if (screen == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_remove_flag(
        screen,
        LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(0x101619),
        LV_PART_MAIN);

    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        LV_PART_MAIN);

    lv_obj_set_style_border_width(
        screen,
        0,
        LV_PART_MAIN);

    lv_obj_set_style_radius(
        screen,
        0,
        LV_PART_MAIN);

    lv_obj_set_style_pad_all(
        screen,
        0,
        LV_PART_MAIN);

    lv_obj_t *title =
        lv_label_create(screen);

    s_reset_state_indicator =
        lv_obj_create(screen);

    s_reset_status_label =
        lv_label_create(screen);

    s_reset_detail_label =
        lv_label_create(screen);

    if ((title == NULL) ||
        (s_reset_state_indicator == NULL) ||
        (s_reset_status_label == NULL) ||
        (s_reset_detail_label == NULL))
    {
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(
        title,
        "Factory Reset");

    lv_obj_set_style_text_font(
        title,
        &lv_font_montserrat_18,
        LV_PART_MAIN);

    lv_obj_set_style_text_color(
        title,
        lv_color_hex(0xF2F5F7),
        LV_PART_MAIN);

    lv_obj_align(
        title,
        LV_ALIGN_CENTER,
        0,
        -46);

    lv_obj_remove_style_all(
        s_reset_state_indicator);

    lv_obj_set_size(
        s_reset_state_indicator,
        18,
        18);

    lv_obj_set_style_radius(
        s_reset_state_indicator,
        LV_RADIUS_CIRCLE,
        LV_PART_MAIN);

    lv_obj_set_style_bg_opa(
        s_reset_state_indicator,
        LV_OPA_COVER,
        LV_PART_MAIN);

    lv_obj_set_style_bg_color(
        s_reset_state_indicator,
        lv_color_hex(0xFFC857),
        LV_PART_MAIN);

    lv_obj_align(
        s_reset_state_indicator,
        LV_ALIGN_CENTER,
        0,
        -18);

    lv_label_set_text(
        s_reset_status_label,
        "Completing...");

    lv_obj_set_style_text_font(
        s_reset_status_label,
        &lv_font_montserrat_18,
        LV_PART_MAIN);

    lv_obj_set_style_text_color(
        s_reset_status_label,
        lv_color_hex(0xFFC857),
        LV_PART_MAIN);

    lv_obj_align(
        s_reset_status_label,
        LV_ALIGN_CENTER,
        0,
        12);

    lv_label_set_text(
        s_reset_detail_label,
        "Please wait");

    lv_label_set_long_mode(
        s_reset_detail_label,
        LV_LABEL_LONG_WRAP);

    lv_obj_set_size(
        s_reset_detail_label,
        148,
        32);

    lv_obj_set_style_text_font(
        s_reset_detail_label,
        &lv_font_montserrat_10,
        LV_PART_MAIN);

    lv_obj_set_style_text_color(
        s_reset_detail_label,
        lv_color_hex(0xA6B0B6),
        LV_PART_MAIN);

    lv_obj_set_style_text_align(
        s_reset_detail_label,
        LV_TEXT_ALIGN_CENTER,
        LV_PART_MAIN);

    lv_obj_align(
        s_reset_detail_label,
        LV_ALIGN_CENTER,
        0,
        43);

    return ESP_OK;
}

static bool app_gui_render_reset_status(
    const ui_reset_status_t *status)
{
    if (!app_gui_is_valid_reset_status(status) ||
        (s_reset_status_label == NULL) ||
        (s_reset_detail_label == NULL) ||
        (s_reset_state_indicator == NULL))
    {
        return false;
    }

    const bool success =
        status->state ==
        UI_RESET_STATE_SUCCESS;

    const lv_color_t state_color =
        success
            ? lv_color_hex(0x49C978)
            : lv_color_hex(0xF06464);

    lv_obj_set_style_bg_color(
        s_reset_state_indicator,
        state_color,
        LV_PART_MAIN);

    lv_obj_set_style_text_color(
        s_reset_status_label,
        state_color,
        LV_PART_MAIN);

    if (success)
    {
        lv_label_set_text(
            s_reset_status_label,
            "Wi-Fi cleared");

        lv_label_set_text(
            s_reset_detail_label,
            "Restarting into setup...");
    }
    else
    {
        char error_text[72] = {0};

        (void)snprintf(
            error_text,
            sizeof(error_text),
            "Error: %s\nRelease and try again",
            esp_err_to_name(
                status->last_error));

        lv_label_set_text(
            s_reset_status_label,
            "Reset failed");

        lv_label_set_text(
            s_reset_detail_label,
            error_text);
    }

    return true;
}

static void app_gui_acknowledge_reset_presentation(void)
{
    uint32_t presented_transaction_id = 0U;

    taskENTER_CRITICAL(
        &s_screen_id_lock);

    if ((s_pending_reset_presentation_id != 0U) &&
        (s_current_screen_id ==
         APP_GUI_SCREEN_RESET_RESULT) &&
        s_latest_reset_status_available &&
        (s_latest_reset_status.transaction_id ==
         s_pending_reset_presentation_id))
    {
        s_presented_reset_transaction_id =
            s_pending_reset_presentation_id;

        presented_transaction_id =
            s_presented_reset_transaction_id;
    }

    /* A pending presentation is valid for only this handler pass. */
    s_pending_reset_presentation_id = 0U;

    taskEXIT_CRITICAL(
        &s_screen_id_lock);

    if (presented_transaction_id != 0U)
    {
        ESP_LOGD(
            TAG,
            "Reset-result presentation acknowledged: transaction=%lu",
            (unsigned long)presented_transaction_id);
    }
}

/* Cached Model Rendering --------------------------------------------------- */
static void app_gui_render_wifi_status(
    const ui_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if ((s_wifi_mode_label == NULL) ||
        (s_wifi_ssid_label == NULL) ||
        (s_wifi_ip_label == NULL)) {
        return;
    }

    lv_label_set_text(
        s_wifi_mode_label,
        app_gui_wifi_state_to_string(status->state));
    lv_obj_set_style_text_color(
        s_wifi_mode_label,
        app_gui_wifi_state_color(status->state),
        LV_PART_MAIN);

    lv_label_set_text(
        s_wifi_ssid_label,
        status->ssid[0] != '\0'
            ? status->ssid
            : "-");

    lv_label_set_text(
        s_wifi_ip_label,
        status->has_ipv4_address &&
        (status->ipv4_address[0] != '\0')
            ? status->ipv4_address
            : "-");
}

static void app_gui_render_sensor_status(
    const ui_sensor_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if ((s_sensor_temperature_label == NULL) ||
        (s_sensor_humidity_label == NULL) ||
        (s_sensor_state_label == NULL)) {
        return;
    }

    if (status->data_valid && !status->data_stale) {
        char temperature_text[16] = {0};
        char humidity_text[16] = {0};

        if (status->temperature_c != APP_GUI_SENSOR_FAILED_VALUE)
        {
            (void)snprintf(
                temperature_text,
                sizeof(temperature_text),
                "%.1f " "\xC2\xB0" "C",
                status->temperature_c);
        }
        else
        {
            (void)snprintf(
                temperature_text,
                sizeof(temperature_text),
                "-");
        }

        if (status->humidity_percent != APP_GUI_SENSOR_FAILED_VALUE)
        {
            (void)snprintf(
                humidity_text,
                sizeof(humidity_text),
                "%.0f %%RH",
                status->humidity_percent);
        }
        else
        {
            (void)snprintf(
                humidity_text,
                sizeof(humidity_text),
                "-");
        }

        lv_label_set_text(
            s_sensor_temperature_label,
            temperature_text);
        lv_label_set_text(
            s_sensor_humidity_label,
            humidity_text);
    }
    else {
        lv_label_set_text(s_sensor_temperature_label, "-");
        lv_label_set_text(s_sensor_humidity_label, "-");
    }

    const ui_sensor_state_t displayed_state =
        status->data_stale
            ? UI_SENSOR_STATE_DEGRADED
            : status->state;

    lv_label_set_text(
        s_sensor_state_label,
        status->data_stale
            ? "Sensor: Stale"
            : app_gui_sensor_state_to_string(status->state));
    lv_obj_set_style_text_color(
        s_sensor_state_label,
        app_gui_sensor_state_color(displayed_state),
        LV_PART_MAIN);
}

static void app_gui_render_audio_status(
    const ui_audio_status_t *status)
{
    if ((status == NULL) || (s_sensor_audio_label == NULL))
    {
        return;
    }

    lv_label_set_text(
        s_sensor_audio_label,
        app_gui_audio_status_to_string(status));
    lv_obj_set_style_text_color(
        s_sensor_audio_label,
        app_gui_audio_status_color(status),
        LV_PART_MAIN);
}

static void app_gui_render_sensor_wifi_status(
    const ui_wifi_status_t *status)
{
    if ((status == NULL) ||
        (s_sensor_wifi_label == NULL) ||
        (s_sensor_wifi_dot == NULL)) {
        return;
    }

    const bool online =
        status->state == UI_WIFI_STATE_CONNECTED &&
        status->has_ipv4_address;
    const lv_color_t state_color =
        online
            ? lv_color_hex(0x49C978)
            : lv_color_hex(0xF06464);

    lv_label_set_text(
        s_sensor_wifi_label,
        online
            ? "Wi-Fi: Online"
            : "Wi-Fi: Offline");
    lv_obj_set_style_text_color(
        s_sensor_wifi_label,
        state_color,
        LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        s_sensor_wifi_dot,
        state_color,
        LV_PART_MAIN);
}

static void app_gui_render_cloud_status(
    const ui_cloud_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if ((s_sensor_cloud_label != NULL) &&
        (s_sensor_cloud_dot != NULL)) {
        const lv_color_t state_color =
            app_gui_cloud_state_color(status->state);

        lv_label_set_text(
            s_sensor_cloud_label,
            app_gui_cloud_state_to_string(status->state));
        lv_obj_set_style_text_color(
            s_sensor_cloud_label,
            state_color,
            LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            s_sensor_cloud_dot,
            state_color,
            LV_PART_MAIN);
    }
}

static bool app_gui_render_cached_status(
    app_gui_screen_id_t screen_id)
{

    bool reset_available = false;

    ui_reset_status_t reset_status =
    {
        .transaction_id = 0U,
        .state = UI_RESET_STATE_FAILED,
        .last_error = ESP_FAIL,
    };

    bool provisioning_available = false;
    bool provisioning_qr_available = false;
    bool wifi_available = false;
    bool sensor_available = false;
    bool audio_available = false;
    bool cloud_available = false;
    ui_provisioning_status_t provisioning_status = {
        .session_generation = 0U,
        .session_number = 0U,
        .session_limit = 0U,
        .state = UI_PROVISIONING_STATE_STARTING,
        .last_error = ESP_OK,
        .wifi_disconnect_reason = 0U,
    };
    ui_provisioning_qr_payload_t provisioning_qr_payload = {0};
    ui_wifi_status_t wifi_status = {0};
    ui_sensor_status_t sensor_status = {0};
    ui_audio_status_t audio_status = {
        .state = UI_AUDIO_STATE_UNAVAILABLE,
        .last_error = ESP_OK,
    };
    ui_cloud_status_t cloud_status = {0};

    taskENTER_CRITICAL(&s_screen_id_lock);

    reset_available =
        s_latest_reset_status_available;
    reset_status =
        s_latest_reset_status;

    if (screen_id !=
        APP_GUI_SCREEN_RESET_RESULT)
    {
        provisioning_available =
            s_latest_provisioning_status_available;
        provisioning_status =
            s_latest_provisioning_status;

        if (screen_id == APP_GUI_SCREEN_PROVISIONING) {
            provisioning_qr_available =
                s_latest_provisioning_qr_payload_available;
            provisioning_qr_payload =
                s_latest_provisioning_qr_payload;
        }

        wifi_available = s_latest_wifi_status_available;
        wifi_status = s_latest_wifi_status;
        sensor_available = s_latest_sensor_status_available;
        sensor_status = s_latest_sensor_status;
        audio_available = s_latest_audio_status_available;
        audio_status = s_latest_audio_status;
        cloud_available = s_latest_cloud_status_available;
        cloud_status = s_latest_cloud_status;
    }

    taskEXIT_CRITICAL(&s_screen_id_lock);

    if (screen_id ==
        APP_GUI_SCREEN_RESET_RESULT)
    {
        if (!reset_available)
        {
            return false;
        }

        return app_gui_render_reset_status(
            &reset_status);
    }

    if (screen_id == APP_GUI_SCREEN_PROVISIONING) {
        if (!provisioning_available) {
            provisioning_status.state =
                UI_PROVISIONING_STATE_STARTING;
            provisioning_status.last_error = ESP_OK;
            provisioning_status.wifi_disconnect_reason = 0U;
        }

        app_gui_render_provisioning_status(
            &provisioning_status);

        if (provisioning_qr_available) {
            app_gui_render_provisioning_qr_payload(
                &provisioning_qr_payload);
        }

        app_gui_zeroize(
            &provisioning_qr_payload,
            sizeof(provisioning_qr_payload));

        return true;
    }

    if ((screen_id == APP_GUI_SCREEN_WIFI_STATUS) &&
        wifi_available) {
        app_gui_render_wifi_status(&wifi_status);
        return true;
    }

    if (screen_id != APP_GUI_SCREEN_SENSOR_DASHBOARD) {
        return true;
    }

    if (wifi_available) {
        app_gui_render_sensor_wifi_status(&wifi_status);
    }

    if (sensor_available) {
        app_gui_render_sensor_status(&sensor_status);
    }

    if (audio_available) {
        app_gui_render_audio_status(&audio_status);
    }

    if (cloud_available) {
        app_gui_render_cloud_status(&cloud_status);
    }

    return true;
}

/* Central Screen Router ---------------------------------------------------- */
static esp_err_t app_gui_activate_screen(
    app_gui_screen_id_t target_screen)
{
    if (!app_gui_is_valid_screen_id(target_screen, false)) {
        return ESP_ERR_INVALID_ARG;
    }

    app_gui_screen_id_t current_screen = APP_GUI_SCREEN_NONE;
    (void)app_gui_get_screen_id(&current_screen);

    ui_manager_lvgl_wait_for_mutex();

    if (target_screen == current_screen) {
        const bool rendered =
            app_gui_render_cached_status(
                target_screen);

        ui_manager_lvgl_release_mutex();

        if (!rendered)
        {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(
            TAG,
            "Ignoring duplicate screen request: %s",
            app_gui_screen_id_to_string(target_screen));
        return ESP_OK;
    }

    lv_obj_t *current_root = lv_screen_active();

    if (current_root == NULL) {
        ui_manager_lvgl_release_mutex();
        return ESP_ERR_INVALID_STATE;
    }

    app_gui_widget_refs_t previous_refs = {0};
    app_gui_capture_widget_refs(&previous_refs);
    app_gui_clear_widget_refs();

    lv_obj_t *target_root = lv_obj_create(NULL);

    if (target_root == NULL) {
        app_gui_apply_widget_refs(&previous_refs);
        ui_manager_lvgl_release_mutex();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;

    switch (target_screen) {
        case APP_GUI_SCREEN_BOOT:
            ret = app_gui_create_boot_screen(target_root);
            break;

        case APP_GUI_SCREEN_PROVISIONING:
            ret = app_gui_create_provisioning_screen(target_root);
            break;

        case APP_GUI_SCREEN_WIFI_STATUS:
            ret = app_gui_create_wifi_screen(target_root);
            break;

        case APP_GUI_SCREEN_SENSOR_DASHBOARD:
            ret = app_gui_create_sensor_screen(target_root);
            break;

        case APP_GUI_SCREEN_RESET_RESULT:
            ret =
                app_gui_create_reset_result_screen(
                    target_root);
            break;

        case APP_GUI_SCREEN_NONE:
        default:
            ret = ESP_ERR_INVALID_ARG;
            break;
    }

    if ((ret == ESP_OK) &&
        (target_screen == APP_GUI_SCREEN_WIFI_STATUS) &&
        (s_wifi_screen_timer == NULL)) {
        s_wifi_screen_timer = lv_timer_create(
            app_gui_wifi_screen_timeout_cb,
            APP_GUI_WIFI_SCREEN_TIMEOUT_MS,
            NULL);

        if (s_wifi_screen_timer == NULL) {
            ret = ESP_ERR_NO_MEM;
        }
    }

    if (ret != ESP_OK) {
        lv_obj_delete(target_root);
        app_gui_apply_widget_refs(&previous_refs);
        ui_manager_lvgl_release_mutex();
        return ret;
    }

    if (!app_gui_render_cached_status(
            target_screen))
    {
        lv_obj_delete(target_root);
        app_gui_apply_widget_refs(
            &previous_refs);
        ui_manager_lvgl_release_mutex();
        return ESP_ERR_INVALID_STATE;
    }

    if ((s_wifi_screen_timer != NULL) &&
        (target_screen != APP_GUI_SCREEN_WIFI_STATUS)) {
        lv_timer_pause(s_wifi_screen_timer);
    }

    lv_screen_load(target_root);
    app_gui_set_active_screen_id(target_screen);

    if (target_screen == APP_GUI_SCREEN_WIFI_STATUS) {
        app_gui_restart_wifi_screen_timer();
    }

    lv_obj_delete(current_root);
    ui_manager_lvgl_release_mutex();

    ESP_LOGI(
        TAG,
        "Application screen transition: %s -> %s",
        app_gui_screen_id_to_string(current_screen),
        app_gui_screen_id_to_string(target_screen));

    return ESP_OK;
}

static void app_gui_process_provisioning_status(
    const ui_provisioning_status_t *status)
{
    if ((status == NULL) ||
        (status->session_generation == 0U) ||
        (status->session_number == 0U) ||
        (status->session_limit == 0U) ||
        (status->session_number >
         status->session_limit) ||
        !app_gui_is_valid_provisioning_state(status->state)) {
        ESP_LOGW(TAG, "Ignoring invalid provisioning GUI status");
        return;
    }

    bool is_newer_generation = false;

    if (!app_gui_accept_provisioning_generation(
            status->session_generation,
            &is_newer_generation))
    {
        ESP_LOGD(
            TAG,
            "Ignoring stale provisioning status generation %lu",
            (unsigned long)status->session_generation);
        return;
    }

    if (is_newer_generation)
    {
        app_gui_hide_provisioning_qr_if_active();
    }

    taskENTER_CRITICAL(&s_screen_id_lock);
    s_latest_provisioning_status = *status;
    s_latest_provisioning_status_available = true;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) == ESP_OK) &&
        (screen_id == APP_GUI_SCREEN_PROVISIONING)) {
        ui_manager_lvgl_wait_for_mutex();
        app_gui_render_provisioning_status(status);
        ui_manager_lvgl_release_mutex();
    }

    ESP_LOGD(
        TAG,
        "GUI received provisioning status: "
        "generation=%lu, session=%lu/%lu, state=%s, "
        "error=%s, disconnect_reason=%u",
        (unsigned long)status->session_generation,
        (unsigned long)status->session_number,
        (unsigned long)status->session_limit,
        app_gui_provisioning_state_to_string(status->state),
        esp_err_to_name(status->last_error),
        (unsigned int)status->wifi_disconnect_reason);
}

static void app_gui_process_provisioning_status_queue(void)
{
    ui_provisioning_status_t status = {0};

    if ((s_provisioning_status_queue == NULL) ||
        (xQueueReceive(
            s_provisioning_status_queue,
            &status,
            0) != pdTRUE)) {
        return;
    }

    app_gui_process_provisioning_status(&status);
}
static void app_gui_process_commands(void)
{
    app_gui_command_t command = {0};

    if (s_command_queue == NULL)
    {
        return;
    }

    while (xQueueReceive(
               s_command_queue,
               &command,
               0U) == pdTRUE)
    {
        switch (command.type)
        {
            case APP_GUI_COMMAND_SHOW_SCREEN:
            {
                bool reset_route_locked = false;

                taskENTER_CRITICAL(
                    &s_screen_id_lock);

                reset_route_locked =
                    s_reset_result_route_locked;

                taskEXIT_CRITICAL(
                    &s_screen_id_lock);

                if (reset_route_locked)
                {
                    ESP_LOGD(
                        TAG,
                        "Screen request ignored during reset-result flow");

                    break;
                }

                const app_gui_screen_id_t target_screen =
                    command.payload.screen_id;

                const esp_err_t error =
                    app_gui_activate_screen(
                        target_screen);

                if (error != ESP_OK)
                {
                    ESP_LOGE(
                        TAG,
                        "Failed to activate screen %s: %s",
                        app_gui_screen_id_to_string(
                            target_screen),
                        esp_err_to_name(error));
                }

                break;
            }

            case APP_GUI_COMMAND_SHOW_RESET_RESULT:
            {
                const ui_reset_status_t *reset_status =
                    &command.payload.reset_status;

                if (!app_gui_is_valid_reset_status(
                        reset_status))
                {
                    ESP_LOGW(
                        TAG,
                        "Ignoring invalid reset-result command");

                    break;
                }

                taskENTER_CRITICAL(
                    &s_screen_id_lock);

                s_latest_reset_status =
                    *reset_status;

                s_latest_reset_status_available =
                    true;

                taskEXIT_CRITICAL(
                    &s_screen_id_lock);

                const esp_err_t error =
                    app_gui_activate_screen(
                        APP_GUI_SCREEN_RESET_RESULT);

                if (error != ESP_OK)
                {
                    ESP_LOGE(
                        TAG,
                        "Failed to activate reset-result screen: %s",
                        esp_err_to_name(error));

                    break;
                }

                /*
                 * Lock normal routing only after the reset screen was
                 * successfully created and loaded. Presentation is not
                 * acknowledged until the following LVGL handler pass.
                 */
                taskENTER_CRITICAL(
                    &s_screen_id_lock);

                s_reset_result_route_locked =
                    true;

                s_pending_reset_presentation_id =
                    reset_status->transaction_id;

                taskEXIT_CRITICAL(
                    &s_screen_id_lock);

                ESP_LOGI(
                    TAG,
                    "Reset-result screen loaded: transaction=%lu, state=%s",
                    (unsigned long)
                        reset_status->transaction_id,
                    app_gui_reset_state_to_string(
                        reset_status->state));

                break;
            }

            default:
            {
                ESP_LOGW(
                    TAG,
                    "Ignoring unknown GUI command: %d",
                    (int)command.type);

                break;
            }
        }
    }
}

static void app_gui_process_provisioning_qr_payload(void)
{
    app_gui_provisioning_qr_message_t message = {0};

    if ((s_provisioning_qr_queue == NULL) ||
        (xQueueReceive(
            s_provisioning_qr_queue,
            &message,
            0) != pdTRUE)) {
        return;
    }

    bool is_newer_generation = false;

    if (!app_gui_accept_provisioning_generation(
            message.payload.session_generation,
            &is_newer_generation))
    {
        ESP_LOGD(
            TAG,
            "Ignoring stale provisioning QR message generation %lu",
            (unsigned long)
                message.payload.session_generation);

        app_gui_zeroize(
            &message,
            sizeof(message));
        return;
    }

    if (is_newer_generation)
    {
        app_gui_hide_provisioning_qr_if_active();
    }

    if (!message.payload_available) {
        taskENTER_CRITICAL(&s_screen_id_lock);
        app_gui_zeroize(
            &s_latest_provisioning_qr_payload,
            sizeof(s_latest_provisioning_qr_payload));
        s_latest_provisioning_qr_payload_available = false;
        taskEXIT_CRITICAL(&s_screen_id_lock);

        app_gui_hide_provisioning_qr_if_active();

        app_gui_zeroize(
            &message,
            sizeof(message));

        ESP_LOGD(
            TAG,
            "Provisioning QR payload cleared for generation %lu",
            (unsigned long)
                s_latest_provisioning_generation);
        return;
    }

    if (!app_gui_is_valid_provisioning_qr_payload(
            &message.payload)) {
        ESP_LOGW(TAG, "Ignoring invalid provisioning QR payload");
        app_gui_zeroize(
            &message,
            sizeof(message));
        return;
    }

    taskENTER_CRITICAL(&s_screen_id_lock);
    s_latest_provisioning_qr_payload = message.payload;
    s_latest_provisioning_qr_payload_available = true;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) == ESP_OK) &&
        (screen_id == APP_GUI_SCREEN_PROVISIONING)) {
        ui_manager_lvgl_wait_for_mutex();
        app_gui_render_provisioning_qr_payload(
            &message.payload);
        ui_manager_lvgl_release_mutex();
    }

    ESP_LOGD(
        TAG,
        "Provisioning QR payload cached for generation %lu",
        (unsigned long)
            message.payload.session_generation);

    app_gui_zeroize(
        &message,
        sizeof(message));
}

/* Sensor Queue Processing ------------------------------------------------- */
static void app_gui_process_sensor_status(void)
{
    ui_sensor_status_t sensor_status = {0};

    if ((s_sensor_status_queue == NULL) ||
        (xQueueReceive(
            s_sensor_status_queue,
            &sensor_status,
            0) != pdTRUE)) {
        return;
    }

    taskENTER_CRITICAL(&s_screen_id_lock);
    s_latest_sensor_status = sensor_status;
    s_latest_sensor_status_available = true;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) == ESP_OK) &&
        (screen_id == APP_GUI_SCREEN_SENSOR_DASHBOARD)) {
        ui_manager_lvgl_wait_for_mutex();
        app_gui_render_sensor_status(&sensor_status);
        ui_manager_lvgl_release_mutex();
    }

    ESP_LOGD(
        TAG,
        "GUI received sensor status: \
        state=%d, temperature=%.1f C, \
        humidity=%.1f, valid=%d, \
        stale=%d, error=%s",
        (int)sensor_status.state,
        sensor_status.temperature_c,
        sensor_status.humidity_percent,
        (int)sensor_status.data_valid,
        (int)sensor_status.data_stale,
        esp_err_to_name(sensor_status.last_error)
    );
}

/* Audio Queue Processing -------------------------------------------------- */
static void app_gui_process_audio_status(void)
{
    ui_audio_status_t audio_status = {
        .state = UI_AUDIO_STATE_UNAVAILABLE,
        .last_error = ESP_OK,
    };

    if ((s_audio_status_queue == NULL) ||
        (xQueueReceive(
            s_audio_status_queue,
            &audio_status,
            0) != pdTRUE))
    {
        return;
    }

    taskENTER_CRITICAL(&s_screen_id_lock);
    s_latest_audio_status = audio_status;
    s_latest_audio_status_available = true;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) == ESP_OK) &&
        (screen_id == APP_GUI_SCREEN_SENSOR_DASHBOARD))
    {
        ui_manager_lvgl_wait_for_mutex();
        app_gui_render_audio_status(&audio_status);
        ui_manager_lvgl_release_mutex();
    }
}

/* Cloud Queue Processing ------------------------------------------------- */
static void app_gui_process_cloud_status(void)
{
    ui_cloud_status_t cloud_status = {0};

    if ((s_cloud_status_queue == NULL) ||
        (xQueueReceive(
            s_cloud_status_queue,
            &cloud_status,
            0) != pdTRUE)) {
        return;
    }

    taskENTER_CRITICAL(&s_screen_id_lock);
    s_latest_cloud_status_available = true;
    s_latest_cloud_status = cloud_status;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) == ESP_OK) &&
        (screen_id == APP_GUI_SCREEN_SENSOR_DASHBOARD)) {
        ui_manager_lvgl_wait_for_mutex();
        app_gui_render_cloud_status(&cloud_status);
        ui_manager_lvgl_release_mutex();
    }

    ESP_LOGD(
        TAG,
        "GUI received cloud status: state=%d, error=%s, HTTP=%d",
        (int)cloud_status.state,
        esp_err_to_name(cloud_status.last_error),
        cloud_status.last_http_status);
}

/* Wi-Fi Queue Processing -------------------------------------------------- */
static void app_gui_process_wifi_status(void)
{
    ui_wifi_status_t wifi_status = {0};

    if ((s_wifi_status_queue == NULL) ||
        (xQueueReceive(
            s_wifi_status_queue,
            &wifi_status,
            0) != pdTRUE)) {
        return;
    }

    taskENTER_CRITICAL(&s_screen_id_lock);
    s_latest_wifi_status_available = true;
    s_latest_wifi_status = wifi_status;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if (app_gui_get_screen_id(&screen_id) == ESP_OK) {
        if (screen_id == APP_GUI_SCREEN_WIFI_STATUS) {
            ui_manager_lvgl_wait_for_mutex();
            app_gui_render_wifi_status(&wifi_status);
            app_gui_restart_wifi_screen_timer();
            ui_manager_lvgl_release_mutex();
        }
        else if (screen_id == APP_GUI_SCREEN_SENSOR_DASHBOARD) {
            ui_manager_lvgl_wait_for_mutex();
            app_gui_render_sensor_wifi_status(&wifi_status);
            ui_manager_lvgl_release_mutex();
        }
    }

    ESP_LOGD(
        TAG,
        "GUI received Wi-Fi status: "
        "state=%d, ip=%s, rssi=%d",
        (int)wifi_status.state,
        wifi_status.has_ipv4_address
            ? wifi_status.ipv4_address
            : "<none>",
        wifi_status.rssi_valid
            ? (int)wifi_status.rssi_dbm
            : 0
    );

}

/* GUI Task Core ----------------------------------------------------------- */
static void app_gui_log_stack_usage(const char *task_name)
{
    const UBaseType_t minimum_free_stack =
        uxTaskGetStackHighWaterMark(NULL);

    if (minimum_free_stack < APP_GUI_STACK_WARNING_BYTES) {
        ESP_LOGW(TAG,
                 "%s minimum free stack is low: %u bytes",
                 task_name,
                 (unsigned int)minimum_free_stack);
    }
    else {
        ESP_LOGD(TAG,
                 "%s minimum free stack: %u bytes",
                 task_name,
                 (unsigned int)minimum_free_stack);
    }
}

static void app_gui_process_lvgl(void)
{
    /* Route screens before status rendering and LVGL timer callbacks. */
    app_gui_process_commands();

    app_gui_process_provisioning_qr_payload();

    app_gui_process_provisioning_status_queue();

    app_gui_process_sensor_status();

    app_gui_process_audio_status();

    app_gui_process_cloud_status();

    app_gui_process_wifi_status();

    ui_manager_lvgl_wait_for_mutex();

    lv_timer_handler();

    ui_manager_lvgl_release_mutex();

    app_gui_acknowledge_reset_presentation();
}

static void app_gui_ui_task(void *param)
{
    (void)param;

    TickType_t last_stack_log = xTaskGetTickCount();

    while (true) {
        app_gui_process_lvgl();

        const TickType_t now = xTaskGetTickCount();
        if ((now - last_stack_log) >=
            pdMS_TO_TICKS(APP_GUI_STACK_LOG_PERIOD_MS)) {
            app_gui_log_stack_usage("Application GUI task");
            last_stack_log = now;
        }

        vTaskDelay(pdMS_TO_TICKS(APP_GUI_TASK_PERIOD_MS));
    }
}

/* Functions ---------------------------------------------------------------- */
/* Lifecycle ---------------------------------------------------------------- */
esp_err_t app_gui_init(void)
{
    if ((s_command_queue != NULL) ||
        (s_provisioning_status_queue != NULL) ||
        (s_provisioning_qr_queue != NULL) ||
        (s_wifi_status_queue != NULL) ||
        (s_sensor_status_queue != NULL) ||
        (s_audio_status_queue != NULL) ||
        (s_cloud_status_queue != NULL)) {
        ESP_LOGW(TAG, "Application GUI is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_command_queue =
        xQueueCreateWithCaps(
            APP_GUI_COMMAND_QUEUE_LENGTH,
            sizeof(app_gui_command_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create GUI command queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_provisioning_status_queue =
        xQueueCreateWithCaps(
            APP_GUI_PROVISIONING_STATUS_QUEUE_LENGTH,
            sizeof(ui_provisioning_status_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_provisioning_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning status queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_provisioning_qr_queue =
        xQueueCreateWithCaps(
            APP_GUI_PROVISIONING_QR_QUEUE_LENGTH,
            sizeof(app_gui_provisioning_qr_message_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_provisioning_qr_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create provisioning QR queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_wifi_status_queue =
        xQueueCreateWithCaps(
            APP_GUI_WIFI_STATUS_QUEUE_LENGTH,
            sizeof(ui_wifi_status_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (s_wifi_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi GUI status queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_sensor_status_queue =
        xQueueCreateWithCaps(
            APP_GUI_SENSOR_STATUS_QUEUE_LENGTH,
            sizeof(ui_sensor_status_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (s_sensor_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Sensor GUI status queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_audio_status_queue =
        xQueueCreateWithCaps(
            APP_GUI_AUDIO_STATUS_QUEUE_LENGTH,
            sizeof(ui_audio_status_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (s_audio_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create audio GUI status queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_cloud_status_queue =
        xQueueCreateWithCaps(
            APP_GUI_CLOUD_STATUS_QUEUE_LENGTH,
            sizeof(ui_cloud_status_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (s_cloud_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Cloud GUI status queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Application GUI initialized");

    return ESP_OK;
}

esp_err_t app_gui_start_ui_task(void)
{
    if ((s_command_queue == NULL) ||
        (s_provisioning_status_queue == NULL) ||
        (s_provisioning_qr_queue == NULL) ||
        (s_wifi_status_queue == NULL) ||
        (s_sensor_status_queue == NULL) ||
        (s_audio_status_queue == NULL) ||
        (s_cloud_status_queue == NULL)) {
        ESP_LOGE(TAG, "Application GUI is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ui_task_handle != NULL) {
        ESP_LOGW(TAG, "Application GUI task is already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(
        app_gui_ui_task,
        "app_gui_ui",
        APP_GUI_UI_TASK_STACK_SIZE_BYTES,
        NULL,
        APP_GUI_TASK_PRIORITY,
        &s_ui_task_handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (task_ret != pdPASS) {
        s_ui_task_handle = NULL;
        ESP_LOGE(TAG,
                 "Failed to create application GUI task with %u-byte stack",
                 (unsigned int)APP_GUI_UI_TASK_STACK_SIZE_BYTES);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Application GUI task started with %u-byte stack",
             (unsigned int)APP_GUI_UI_TASK_STACK_SIZE_BYTES);

    return ESP_OK;
}

/* Provisioning Status API ------------------------------------------------- */
esp_err_t app_gui_post_provisioning_status(
    const ui_provisioning_status_t *status)
{
    if ((status == NULL) ||
        (status->session_generation == 0U) ||
        (status->session_number == 0U) ||
        (status->session_limit == 0U) ||
        (status->session_number >
         status->session_limit) ||
        !app_gui_is_valid_provisioning_state(status->state)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_provisioning_status_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!app_gui_accept_queued_provisioning_generation(
            status->session_generation))
    {
        ESP_LOGD(
            TAG,
            "Rejecting stale queued provisioning status generation %lu",
            (unsigned long)status->session_generation);
        return ESP_OK;
    }

    if (xQueueOverwrite(
            s_provisioning_status_queue,
            status) != pdTRUE) {
        ESP_LOGW(
            TAG,
            "Failed to post provisioning status %s",
            app_gui_provisioning_state_to_string(status->state));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_gui_post_provisioning_qr_payload(
    const ui_provisioning_qr_payload_t *payload)
{
    if (!app_gui_is_valid_provisioning_qr_payload(payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_provisioning_qr_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!app_gui_accept_queued_provisioning_generation(
            payload->session_generation))
    {
        ESP_LOGD(
            TAG,
            "Rejecting stale queued provisioning QR generation %lu",
            (unsigned long)payload->session_generation);
        return ESP_OK;
    }

    const app_gui_provisioning_qr_message_t message = {
        .payload_available = true,
        .payload = *payload,
    };

    if (xQueueOverwrite(
            s_provisioning_qr_queue,
            &message) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to post provisioning QR payload");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_gui_clear_provisioning_qr_payload(
    uint32_t session_generation)
{
    if (session_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_provisioning_qr_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!app_gui_accept_queued_provisioning_generation(
            session_generation))
    {
        ESP_LOGD(
            TAG,
            "Rejecting stale queued provisioning QR clear generation %lu",
            (unsigned long)session_generation);
        return ESP_OK;
    }

    const app_gui_provisioning_qr_message_t message = {
        .payload_available = false,
        .payload = {
            .session_generation = session_generation,
            .payload = {0},
        },
    };

    if (xQueueOverwrite(
            s_provisioning_qr_queue,
            &message) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to clear provisioning QR payload");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Status Queue API --------------------------------------------------------- */
esp_err_t app_gui_post_wifi_status(
    const ui_wifi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_wifi_status_queue == NULL) {
        ESP_LOGE(TAG, "Wi-Fi GUI queue is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Queue length = 1:
     * UI only needs the newest Wi-Fi state.
     *
     * No waiting because this function can be called from
     * the ESP event-loop task through the application callback.
     */
    if (xQueueOverwrite(
            s_wifi_status_queue,
            status) != pdTRUE) {

        ESP_LOGW(TAG, "Failed to post Wi-Fi status to UI");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_gui_post_sensor_status(
    const ui_sensor_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sensor_status_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(
            s_sensor_status_queue,
            status,
            0) != pdPASS)
    {
        /*
         * Sensor data is periodic, so dropping one update is acceptable.
         * The next sample will deliver a newer snapshot.
         */
        ESP_LOGE(TAG, "GUI Sensor queue updating TimeOut");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t app_gui_post_audio_status(
    const ui_audio_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_audio_status_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueOverwrite(
            s_audio_status_queue,
            status) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to post audio status to UI");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_gui_post_cloud_status(
    const ui_cloud_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_cloud_status_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueOverwrite(
            s_cloud_status_queue,
            status) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to post Cloud status to UI");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Screen Routing API ------------------------------------------------------- */
esp_err_t app_gui_request_screen(
    app_gui_screen_id_t screen_id)
{
    if (!app_gui_is_valid_screen_id(
            screen_id,
            false) ||
        (screen_id ==
         APP_GUI_SCREEN_RESET_RESULT))
    {
        ESP_LOGW(
            TAG,
            "Invalid screen request target: %s (%d)",
            app_gui_screen_id_to_string(screen_id),
            (int)screen_id);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const app_gui_command_t command =
    {
        .type =
            APP_GUI_COMMAND_SHOW_SCREEN,

        .payload =
        {
            .screen_id =
                screen_id,
        },
    };

    if (xQueueSend(
            s_command_queue,
            &command,
            0) != pdTRUE) {
        ESP_LOGW(
            TAG,
            "GUI command queue is full; screen %s was not queued",
            app_gui_screen_id_to_string(screen_id));
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t app_gui_get_screen_id(
    app_gui_screen_id_t *screen_id)
{
    ESP_RETURN_ON_FALSE(
        screen_id != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Screen ID output pointer is NULL"
    );

    taskENTER_CRITICAL(&s_screen_id_lock);
    *screen_id = s_current_screen_id;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    return ESP_OK;
}

esp_err_t app_gui_show_reset_result(
    const ui_reset_status_t *status)
{
    if (!app_gui_is_valid_reset_status(status))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const app_gui_command_t command =
    {
        .type =
            APP_GUI_COMMAND_SHOW_RESET_RESULT,

        .payload =
        {
            .reset_status =
                *status,
        },
    };

    if (xQueueSend(
            s_command_queue,
            &command,
            0U) != pdTRUE)
    {
        ESP_LOGW(
            TAG,
            "GUI command queue is full; "
            "reset result was not queued");

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t app_gui_is_reset_result_presented(
    uint32_t transaction_id,
    bool *presented)
{
    if ((transaction_id == 0U) ||
        (presented == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *presented = false;

    if (s_command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(
        &s_screen_id_lock);

    *presented =
        s_presented_reset_transaction_id ==
        transaction_id;

    taskEXIT_CRITICAL(
        &s_screen_id_lock);

    return ESP_OK;
}
