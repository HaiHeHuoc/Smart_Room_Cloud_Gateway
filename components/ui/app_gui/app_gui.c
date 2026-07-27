/* Includes ----------------------------------------------------------------- */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ui_manager_lvgl.h"
#include "app_gui.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "lvgl.h"

/* Macros ------------------------------------------------------------------- */
#define APP_GUI_WIFI_SCREEN_TIMEOUT_MS 10000U
#define APP_GUI_COMMAND_QUEUE_LENGTH 8U
#define APP_GUI_WIFI_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_SENSOR_STATUS_QUEUE_LENGTH 5U
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

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    APP_GUI_COMMAND_SHOW_SCREEN = 0,
} app_gui_command_type_t;

typedef struct
{
    app_gui_command_type_t type;

    union
    {
        app_gui_screen_id_t screen_id;
    } data;
} app_gui_command_t;

typedef struct
{
    lv_obj_t *wifi_mode_label;
    lv_obj_t *wifi_ssid_label;
    lv_obj_t *wifi_ip_label;
    lv_obj_t *sensor_temperature_label;
    lv_obj_t *sensor_humidity_label;
    lv_obj_t *sensor_state_label;
    lv_obj_t *sensor_wifi_label;
    lv_obj_t *sensor_wifi_dot;
    lv_obj_t *sensor_cloud_label;
    lv_obj_t *sensor_cloud_dot;
} app_gui_widget_refs_t;

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_GUI";

/* Static Variables --------------------------------------------------------- */
/* GUI task communication and active-screen tracking. */
static QueueHandle_t s_command_queue = NULL;
static QueueHandle_t s_wifi_status_queue = NULL;
static QueueHandle_t s_sensor_status_queue = NULL;
static QueueHandle_t s_cloud_status_queue = NULL;
static TaskHandle_t s_ui_task_handle = NULL;
static app_gui_screen_id_t s_current_screen_id = APP_GUI_SCREEN_NONE;
static portMUX_TYPE s_screen_id_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_latest_wifi_status_available = false;
static ui_wifi_status_t s_latest_wifi_status = {0};
static bool s_latest_sensor_status_available = false;
static ui_sensor_status_t s_latest_sensor_status = {0};
static bool s_latest_cloud_status_available = false;
static ui_cloud_status_t s_latest_cloud_status = {0};

/* Wi-Fi object references are valid only while the Wi-Fi screen is active. */
static lv_obj_t *s_wifi_mode_label = NULL;
static lv_obj_t *s_wifi_ssid_label = NULL;
static lv_obj_t *s_wifi_ip_label = NULL;
static lv_timer_t *s_wifi_screen_timer = NULL;

/* Sensor object references are valid only while the sensor screen is active. */
static lv_obj_t *s_sensor_temperature_label = NULL;
static lv_obj_t *s_sensor_humidity_label = NULL;
static lv_obj_t *s_sensor_state_label = NULL;
static lv_obj_t *s_sensor_wifi_label = NULL;
static lv_obj_t *s_sensor_wifi_dot = NULL;
static lv_obj_t *s_sensor_cloud_label = NULL;
static lv_obj_t *s_sensor_cloud_dot = NULL;

/* Function Prototypes ------------------------------------------------------ */
static bool app_gui_is_valid_screen_id(
    app_gui_screen_id_t screen_id,
    bool allow_none);
static const char *app_gui_screen_id_to_string(
    app_gui_screen_id_t screen_id);
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
static esp_err_t app_gui_create_wifi_screen(
    lv_obj_t *screen);
static esp_err_t app_gui_create_sensor_screen(
    lv_obj_t *screen);
static const char *app_gui_wifi_state_to_string(ui_wifi_state_t state);
static lv_color_t app_gui_wifi_state_color(ui_wifi_state_t state);
static const char *app_gui_sensor_state_to_string(ui_sensor_state_t state);
static lv_color_t app_gui_sensor_state_color(ui_sensor_state_t state);
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
static void app_gui_render_sensor_wifi_status(
    const ui_wifi_status_t *status);
static void app_gui_render_cloud_status(
    const ui_cloud_status_t *status);
static void app_gui_render_cached_status(
    app_gui_screen_id_t screen_id);
static esp_err_t app_gui_activate_screen(
    app_gui_screen_id_t target_screen);
static void app_gui_process_commands(void);
static void app_gui_process_sensor_status(void);
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
        ((allow_none && (screen_id == APP_GUI_SCREEN_NONE)) ||
         (screen_id == APP_GUI_SCREEN_BOOT) ||
         (screen_id == APP_GUI_SCREEN_PROVISIONING) ||
         (screen_id == APP_GUI_SCREEN_WIFI_STATUS) ||
         (screen_id == APP_GUI_SCREEN_SENSOR_DASHBOARD));
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

        default:
            return "UNKNOWN";
    }
}

static void app_gui_cleanup_queues(void)
{
    if (s_command_queue != NULL) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
    }

    if (s_wifi_status_queue != NULL) {
        vQueueDelete(s_wifi_status_queue);
        s_wifi_status_queue = NULL;
    }

    if (s_sensor_status_queue != NULL) {
        vQueueDelete(s_sensor_status_queue);
        s_sensor_status_queue = NULL;
    }

    if (s_cloud_status_queue != NULL) {
        vQueueDelete(s_cloud_status_queue);
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

    refs->wifi_mode_label = s_wifi_mode_label;
    refs->wifi_ssid_label = s_wifi_ssid_label;
    refs->wifi_ip_label = s_wifi_ip_label;
    refs->sensor_temperature_label = s_sensor_temperature_label;
    refs->sensor_humidity_label = s_sensor_humidity_label;
    refs->sensor_state_label = s_sensor_state_label;
    refs->sensor_wifi_label = s_sensor_wifi_label;
    refs->sensor_wifi_dot = s_sensor_wifi_dot;
    refs->sensor_cloud_label = s_sensor_cloud_label;
    refs->sensor_cloud_dot = s_sensor_cloud_dot;
}

static void app_gui_clear_widget_refs(void)
{
    s_wifi_mode_label = NULL;
    s_wifi_ssid_label = NULL;
    s_wifi_ip_label = NULL;
    s_sensor_temperature_label = NULL;
    s_sensor_humidity_label = NULL;
    s_sensor_state_label = NULL;
    s_sensor_wifi_label = NULL;
    s_sensor_wifi_dot = NULL;
    s_sensor_cloud_label = NULL;
    s_sensor_cloud_dot = NULL;
}

static void app_gui_apply_widget_refs(
    const app_gui_widget_refs_t *refs)
{
    app_gui_clear_widget_refs();

    if (refs == NULL) {
        return;
    }

    s_wifi_mode_label = refs->wifi_mode_label;
    s_wifi_ssid_label = refs->wifi_ssid_label;
    s_wifi_ip_label = refs->wifi_ip_label;
    s_sensor_temperature_label = refs->sensor_temperature_label;
    s_sensor_humidity_label = refs->sensor_humidity_label;
    s_sensor_state_label = refs->sensor_state_label;
    s_sensor_wifi_label = refs->sensor_wifi_label;
    s_sensor_wifi_dot = refs->sensor_wifi_dot;
    s_sensor_cloud_label = refs->sensor_cloud_label;
    s_sensor_cloud_dot = refs->sensor_cloud_dot;
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

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_t *status = lv_label_create(screen);

    if ((title == NULL) || (status == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    lv_label_set_text(title, "Wi-Fi Setup");
    lv_obj_set_style_text_font(
        title,
        &lv_font_montserrat_20,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        title,
        lv_color_hex(0xF2F5F7),
        LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -14);

    lv_label_set_text(status, "Preparing setup...");
    lv_obj_set_style_text_font(
        status,
        &lv_font_montserrat_12,
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        status,
        lv_color_hex(0x8C989F),
        LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 14);

    return ESP_OK;
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
        APP_GUI_DASHBOARD_HEIGHT_PX -
            APP_GUI_DASHBOARD_HEADER_HEIGHT_PX - 1);
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
    s_sensor_wifi_label =
        app_gui_create_sensor_value_label(
            screen,
            38,
            !wifi_status_available
                ? "Wi-Fi: --"
                : (wifi_online
                    ? "Wi-Fi: Online"
                    : "Wi-Fi: Offline"));
    s_sensor_cloud_label =
        app_gui_create_sensor_value_label(
            screen,
            68,
            cloud_status_available
                ? app_gui_cloud_state_to_string(cloud_state)
                : "Cloud: --");
    s_sensor_state_label =
        app_gui_create_sensor_value_label(screen, 98, "Sensor: --");

    if ((s_sensor_temperature_label == NULL) ||
        (s_sensor_humidity_label == NULL) ||
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
    };

    for (size_t index = 0U;
         index < (sizeof(right_values) / sizeof(right_values[0]));
         ++index) {
        lv_obj_set_x(right_values[index], APP_GUI_DASHBOARD_RIGHT_X_PX);
        lv_obj_set_width(
            right_values[index],
            APP_GUI_DASHBOARD_RIGHT_WIDTH_PX);
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

    return ESP_OK;
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

static void app_gui_render_cached_status(
    app_gui_screen_id_t screen_id)
{
    bool wifi_available = false;
    bool sensor_available = false;
    bool cloud_available = false;
    ui_wifi_status_t wifi_status = {0};
    ui_sensor_status_t sensor_status = {0};
    ui_cloud_status_t cloud_status = {0};

    taskENTER_CRITICAL(&s_screen_id_lock);
    wifi_available = s_latest_wifi_status_available;
    wifi_status = s_latest_wifi_status;
    sensor_available = s_latest_sensor_status_available;
    sensor_status = s_latest_sensor_status;
    cloud_available = s_latest_cloud_status_available;
    cloud_status = s_latest_cloud_status;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    if ((screen_id == APP_GUI_SCREEN_WIFI_STATUS) &&
        wifi_available) {
        app_gui_render_wifi_status(&wifi_status);
        return;
    }

    if (screen_id != APP_GUI_SCREEN_SENSOR_DASHBOARD) {
        return;
    }

    if (wifi_available) {
        app_gui_render_sensor_wifi_status(&wifi_status);
    }

    if (sensor_available) {
        app_gui_render_sensor_status(&sensor_status);
    }

    if (cloud_available) {
        app_gui_render_cloud_status(&cloud_status);
    }
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
        app_gui_render_cached_status(target_screen);
        ui_manager_lvgl_release_mutex();
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

    if ((s_wifi_screen_timer != NULL) &&
        (target_screen != APP_GUI_SCREEN_WIFI_STATUS)) {
        lv_timer_pause(s_wifi_screen_timer);
    }

    lv_screen_load(target_root);
    app_gui_set_active_screen_id(target_screen);
    app_gui_render_cached_status(target_screen);

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

static void app_gui_process_commands(void)
{
    app_gui_command_t command = {0};

    if (s_command_queue == NULL) {
        return;
    }

    while (xQueueReceive(
               s_command_queue,
               &command,
               0) == pdTRUE) {
        if (command.type != APP_GUI_COMMAND_SHOW_SCREEN) {
            ESP_LOGW(
                TAG,
                "Ignoring unknown GUI command: %d",
                (int)command.type);
            continue;
        }

        const esp_err_t ret =
            app_gui_activate_screen(
                command.data.screen_id);

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to activate screen %s: %s",
                app_gui_screen_id_to_string(
                    command.data.screen_id),
                esp_err_to_name(ret));
        }
    }
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

    app_gui_process_sensor_status();

    app_gui_process_cloud_status();

    app_gui_process_wifi_status();

    ui_manager_lvgl_wait_for_mutex();

    lv_timer_handler();

    ui_manager_lvgl_release_mutex();
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
        (s_wifi_status_queue != NULL) ||
        (s_sensor_status_queue != NULL) ||
        (s_cloud_status_queue != NULL)) {
        ESP_LOGW(TAG, "Application GUI is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_command_queue =
        xQueueCreate(
            APP_GUI_COMMAND_QUEUE_LENGTH,
            sizeof(app_gui_command_t));

    if (s_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create GUI command queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_wifi_status_queue =
        xQueueCreate(
            APP_GUI_WIFI_STATUS_QUEUE_LENGTH,
            sizeof(ui_wifi_status_t)
        );

    if (s_wifi_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi GUI status queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_sensor_status_queue =
        xQueueCreate(
            APP_GUI_SENSOR_STATUS_QUEUE_LENGTH,
            sizeof(ui_sensor_status_t)
        );

    if (s_sensor_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Sensor GUI status queue");
        app_gui_cleanup_queues();
        return ESP_ERR_NO_MEM;
    }

    s_cloud_status_queue =
        xQueueCreate(
            APP_GUI_CLOUD_STATUS_QUEUE_LENGTH,
            sizeof(ui_cloud_status_t)
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
        (s_wifi_status_queue == NULL) ||
        (s_sensor_status_queue == NULL) ||
        (s_cloud_status_queue == NULL)) {
        ESP_LOGE(TAG, "Application GUI is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ui_task_handle != NULL) {
        ESP_LOGW(TAG, "Application GUI task is already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreate(
        app_gui_ui_task,
        "app_gui_ui",
        APP_GUI_UI_TASK_STACK_SIZE_BYTES,
        NULL,
        APP_GUI_TASK_PRIORITY,
        &s_ui_task_handle
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
    if (!app_gui_is_valid_screen_id(screen_id, false)) {
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

    const app_gui_command_t command = {
        .type = APP_GUI_COMMAND_SHOW_SCREEN,
        .data.screen_id = screen_id,
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
