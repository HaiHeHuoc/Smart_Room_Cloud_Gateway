/* Includes ----------------------------------------------------------------- */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ui_manager_lvgl.h"
#include "board_config.h"
#include "app_gui.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "lvgl.h"

/* Macros ------------------------------------------------------------------- */
#define APP_GUI_WIFI_SCREEN_TIMEOUT_MS 10000U
#define APP_GUI_WIFI_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_SENSOR_STATUS_QUEUE_LENGTH 5U
#define APP_GUI_CLOUD_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_UI_TASK_STACK_SIZE_BYTES   (24U * 1024U)
#define APP_GUI_DEMO_TASK_STACK_SIZE_BYTES (4U * 1024U)
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

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_GUI";

/* Static Variables --------------------------------------------------------- */
/* GUI task communication and active-screen tracking. */
static QueueHandle_t s_wifi_status_queue = NULL;
static QueueHandle_t s_sensor_status_queue = NULL;
static QueueHandle_t s_cloud_status_queue = NULL;
static TaskHandle_t s_ui_task_handle = NULL;
static TaskHandle_t s_demo_task_handle = NULL;
static app_gui_screen_id_t s_current_screen_id = APP_GUI_SCREEN_NONE;
static portMUX_TYPE s_screen_id_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_latest_wifi_status_available = false;
static bool s_latest_wifi_online = false;
static bool s_latest_cloud_status_available = false;
static ui_cloud_state_t s_latest_cloud_state = UI_CLOUD_STATE_UNKNOWN;

/* Wi-Fi object references are valid only while the Wi-Fi screen is active. */
static lv_obj_t *s_wifi_mode_label = NULL;
static lv_obj_t *s_wifi_ssid_label = NULL;
static lv_obj_t *s_wifi_ip_label = NULL;
static lv_timer_t *s_wifi_screen_timer = NULL;

/* Sensor object references are valid only while the sensor screen is active. */
static lv_obj_t *s_sensor_temperature_label = NULL;
static lv_obj_t *s_sensor_humidity_label = NULL;
static lv_obj_t *s_sensor_state_label = NULL;
static lv_obj_t *s_sensor_cloud_label = NULL;
static lv_obj_t *s_sensor_cloud_dot = NULL;

/* Function Prototypes ------------------------------------------------------ */
static void app_gui_wifi_screen_timeout_cb(lv_timer_t *timer);
static void app_gui_restart_wifi_screen_timer(void);
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
static void app_gui_update_wifi_screen(const ui_wifi_status_t *status);
static void app_gui_update_sensor_screen(const ui_sensor_status_t *status);
static void app_gui_update_cloud_status(const ui_cloud_status_t *status);
static void app_gui_process_sensor_status(void);
static void app_gui_process_wifi_status(void);
static void app_gui_process_cloud_status(void);
static void app_gui_log_stack_usage(const char *task_name);
static void app_gui_process_lvgl(void);
static void app_gui_ui_task(void *param);
static void app_gui_running_demo(void *parameter);

/* Application -------------------------------------------------------------- */
void app_gui_create_demo_screen(void)
{
    /* All LVGL object access is serialized through the UI manager mutex. */
    ui_manager_lvgl_wait_for_mutex();

    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        ESP_LOGE(TAG, "No active LVGL screen for demo GUI");
        ui_manager_lvgl_release_mutex();
        return;
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(screen);
    if (label == NULL) {
        ESP_LOGE(TAG, "Failed to create demo label");
        ui_manager_lvgl_release_mutex();
        return;
    }

    lv_label_set_text(label, "LVGL OK");
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_obj_center(label);

    (void)app_gui_set_screen_id(APP_GUI_SCREEN_NONE);

    ui_manager_lvgl_release_mutex();
}

/* Static Functions --------------------------------------------------------- */
/* Wi-Fi Screen Helpers ----------------------------------------------------- */
static void app_gui_wifi_screen_timeout_cb(lv_timer_t *timer)
{
    /*
     * LVGL invokes this callback from lv_timer_handler(), while the app GUI
     * task already owns the LVGL mutex. app_gui_clear_screen() must therefore
     * not attempt to lock that non-recursive mutex again.
     */
    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) != ESP_OK) ||
        (screen_id != APP_GUI_SCREEN_WIFI)) {
        lv_timer_pause(timer);
        return;
    }


    esp_err_t ret = app_gui_clear_screen();
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to clear Wi-Fi screen: %s",
            esp_err_to_name(ret));
    }

    ESP_LOGD(TAG, "Wi-Fi screen timeout");

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

/* Wi-Fi Screen Construction ----------------------------------------------- */
esp_err_t app_gui_create_wifi_screen(void)
{

    ui_manager_lvgl_wait_for_mutex();

    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        ESP_LOGE(TAG, "No active LVGL screen for Wi-Fi GUI");
        ui_manager_lvgl_release_mutex();
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_clean(screen);
    s_wifi_mode_label = NULL;
    s_wifi_ssid_label = NULL;
    s_wifi_ip_label = NULL;
    s_sensor_temperature_label = NULL;
    s_sensor_humidity_label = NULL;
    s_sensor_state_label = NULL;
    s_sensor_cloud_label = NULL;
    s_sensor_cloud_dot = NULL;
    (void)app_gui_set_screen_id(APP_GUI_SCREEN_NONE);

    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,
                              lv_color_hex(0x101619),
                              LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    if (title == NULL) {
        ui_manager_lvgl_release_mutex();
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
        ui_manager_lvgl_release_mutex();
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
            ui_manager_lvgl_release_mutex();
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
        ui_manager_lvgl_release_mutex();
        return ESP_ERR_NO_MEM;
    }

    /* Give the longest IPv4 string more horizontal room than other values. */
    lv_obj_set_pos(s_wifi_ip_label, 32, 93);
    lv_obj_set_width(s_wifi_ip_label, APP_GUI_WIFI_IP_VALUE_WIDTH_PX);

    lv_obj_set_style_text_color(
        s_wifi_mode_label,
        app_gui_wifi_state_color(UI_WIFI_STATE_IDLE),
        LV_PART_MAIN);

    ESP_LOGD(TAG, "Wi-Fi status screen created");

    if (s_wifi_screen_timer == NULL) {
        s_wifi_screen_timer = lv_timer_create(
            app_gui_wifi_screen_timeout_cb,
            APP_GUI_WIFI_SCREEN_TIMEOUT_MS,
            NULL
        );
    }

    (void)app_gui_set_screen_id(APP_GUI_SCREEN_WIFI);
    app_gui_restart_wifi_screen_timer();

    ui_manager_lvgl_release_mutex();
    return ESP_OK;
}

/* Sensor Screen Construction ---------------------------------------------- */
esp_err_t app_gui_create_sensor_screen(void)
{
    ui_manager_lvgl_wait_for_mutex();

    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        ESP_LOGE(TAG, "No active LVGL screen for sensor GUI");
        ui_manager_lvgl_release_mutex();
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_clean(screen);
    s_wifi_mode_label = NULL;
    s_wifi_ssid_label = NULL;
    s_wifi_ip_label = NULL;
    s_sensor_temperature_label = NULL;
    s_sensor_humidity_label = NULL;
    s_sensor_state_label = NULL;
    s_sensor_cloud_label = NULL;
    s_sensor_cloud_dot = NULL;
    (void)app_gui_set_screen_id(APP_GUI_SCREEN_NONE);

    if (s_wifi_screen_timer != NULL) {
        lv_timer_pause(s_wifi_screen_timer);
    }

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
    wifi_online = s_latest_wifi_online;
    cloud_status_available = s_latest_cloud_status_available;
    cloud_state = s_latest_cloud_state;
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
        ui_manager_lvgl_release_mutex();
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
    lv_obj_t *wifi_dot = app_gui_create_dashboard_dot(
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
        (wifi_dot == NULL) ||
        (s_sensor_cloud_dot == NULL)) {
        ui_manager_lvgl_release_mutex();
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
    lv_obj_t *sensor_wifi_label =
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
        (sensor_wifi_label == NULL) ||
        (s_sensor_cloud_label == NULL) ||
        (s_sensor_state_label == NULL)) {
        s_sensor_temperature_label = NULL;
        s_sensor_humidity_label = NULL;
        s_sensor_state_label = NULL;
        s_sensor_cloud_label = NULL;
        s_sensor_cloud_dot = NULL;
        ui_manager_lvgl_release_mutex();
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
        sensor_wifi_label,
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
        sensor_wifi_label,
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

    (void)app_gui_set_screen_id(APP_GUI_SCREEN_SENSOR);
    ESP_LOGD(TAG, "Sensor status screen created");

    ui_manager_lvgl_release_mutex();
    return ESP_OK;
}

/* Wi-Fi Screen Update ------------------------------------------------------ */
static void app_gui_update_wifi_screen(const ui_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    ui_manager_lvgl_wait_for_mutex();

    if ((s_wifi_mode_label == NULL) ||
        (s_wifi_ssid_label == NULL) ||
        (s_wifi_ip_label == NULL)) {
        ui_manager_lvgl_release_mutex();
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

    ui_manager_lvgl_release_mutex();
}

/* Sensor Screen Update ---------------------------------------------------- */
static void app_gui_update_sensor_screen(
    const ui_sensor_status_t *status)
{
    if (status == NULL) {
        return;
    }

    ui_manager_lvgl_wait_for_mutex();

    if ((s_sensor_temperature_label == NULL) ||
        (s_sensor_humidity_label == NULL) ||
        (s_sensor_state_label == NULL)) {
        ui_manager_lvgl_release_mutex();
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

    ui_manager_lvgl_release_mutex();
}

/* Cloud Status Update ----------------------------------------------------- */
static void app_gui_update_cloud_status(
    const ui_cloud_status_t *status)
{
    if (status == NULL) {
        return;
    }

    ui_manager_lvgl_wait_for_mutex();

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

    ui_manager_lvgl_release_mutex();
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

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if (app_gui_get_screen_id(&screen_id) == ESP_OK) {
        if (screen_id == APP_GUI_SCREEN_NONE) {
            ui_manager_lvgl_wait_for_mutex();
            (void)app_gui_clear_screen();
            ui_manager_lvgl_release_mutex();

            const esp_err_t ret = app_gui_create_sensor_screen();
            if (ret != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Failed to create sensor screen: %s",
                    esp_err_to_name(ret));
            }
            else {
                screen_id = APP_GUI_SCREEN_SENSOR;
            }
        }

        if (screen_id == APP_GUI_SCREEN_SENSOR) {
            app_gui_update_sensor_screen(&sensor_status);
        }
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
    s_latest_cloud_state = cloud_status.state;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) == ESP_OK) &&
        (screen_id == APP_GUI_SCREEN_SENSOR)) {
        app_gui_update_cloud_status(&cloud_status);
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

    /* Retain only the summary needed by the compact sensor dashboard. */
    taskENTER_CRITICAL(&s_screen_id_lock);
    s_latest_wifi_status_available = true;
    s_latest_wifi_online =
        wifi_status.state == UI_WIFI_STATE_CONNECTED &&
        wifi_status.has_ipv4_address;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    app_gui_screen_id_t screen_id = APP_GUI_SCREEN_NONE;

    if ((app_gui_get_screen_id(&screen_id) == ESP_OK) &&
        (screen_id != APP_GUI_SCREEN_WIFI))
    {
        ui_manager_lvgl_wait_for_mutex();
        (void)app_gui_clear_screen();
        ui_manager_lvgl_release_mutex();

        (void)app_gui_create_wifi_screen();
    }

    /* LVGL timer operations follow the same mutex contract as LVGL objects. */
    ui_manager_lvgl_wait_for_mutex();
    app_gui_restart_wifi_screen_timer();
    ui_manager_lvgl_release_mutex();

    app_gui_update_wifi_screen(&wifi_status);

    ESP_LOGD(
        TAG,
        "GUI received Wi-Fi status: "
        "state=%d, ssid=%s, ip=%s, rssi=%d",
        (int)wifi_status.state,
        wifi_status.ssid[0] != '\0'
            ? wifi_status.ssid
            : "<none>",
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
    /* Queue handlers do not wait; LVGL timers are then serviced once. */
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

/* Demo Task --------------------------------------------------------------- */
static void app_gui_running_demo(void *parameter)
{
    (void)parameter;

    const lv_align_t state[9] = {   
    LV_ALIGN_TOP_LEFT,
    LV_ALIGN_TOP_MID,
    LV_ALIGN_TOP_RIGHT,
    LV_ALIGN_LEFT_MID,
    LV_ALIGN_CENTER,
    LV_ALIGN_RIGHT_MID,
    LV_ALIGN_BOTTOM_LEFT,
    LV_ALIGN_BOTTOM_MID,
    LV_ALIGN_BOTTOM_RIGHT,
    };

    ui_manager_lvgl_wait_for_mutex();
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        ESP_LOGE(TAG, "No active LVGL screen for running demo");
        ui_manager_lvgl_release_mutex();
        s_demo_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // Set background color to white 
    lv_obj_t *label = lv_label_create(screen);
    if (label == NULL) {
        ESP_LOGE(TAG, "Failed to create running demo label");
        ui_manager_lvgl_release_mutex();
        s_demo_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    lv_label_set_text(label, "LVGL OK"); 
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0); 
    // lv_obj_center(label); 
    lv_obj_set_align(label, state[0]);
    ui_manager_lvgl_release_mutex();

    uint8_t counter = 0;

    while(1) { 
        vTaskDelay(pdMS_TO_TICKS(500)); 
        // Delay for 1 second 
        ui_manager_lvgl_wait_for_mutex();
        if (!lv_obj_is_valid(label)) {
            ESP_LOGW(TAG, "Running demo stopped because its label was deleted");
            ui_manager_lvgl_release_mutex();
            s_demo_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }

        char text[20]; counter = (counter + 1) % 100; 
        // Increment counter and wrap around at 100 
        snprintf(text, sizeof(text), "Counter: %d", counter); 
        lv_label_set_text(label, text);
        // ESP_LOGI(TAG, "Updated label text to: %s", text); 
        lv_obj_set_align(label, state[counter%9]);
        ui_manager_lvgl_release_mutex();
    } 
}

/* Functions ---------------------------------------------------------------- */
/* Lifecycle ---------------------------------------------------------------- */
esp_err_t app_gui_init(void)
{
    if (s_wifi_status_queue != NULL) {
        ESP_LOGW(TAG, "Application GUI is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_status_queue =
        xQueueCreate(
            APP_GUI_WIFI_STATUS_QUEUE_LENGTH,
            sizeof(ui_wifi_status_t)
        );

    ESP_RETURN_ON_FALSE(s_wifi_status_queue != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Failed to create Wi-Fi GUI status queue"
    );

    s_sensor_status_queue =
        xQueueCreate(
            APP_GUI_SENSOR_STATUS_QUEUE_LENGTH,
            sizeof(ui_sensor_status_t)
        );

    ESP_RETURN_ON_FALSE(s_sensor_status_queue != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Failed to create Sensor GUI status queue"
    );

    s_cloud_status_queue =
        xQueueCreate(
            APP_GUI_CLOUD_STATUS_QUEUE_LENGTH,
            sizeof(ui_cloud_status_t)
        );

    ESP_RETURN_ON_FALSE(s_cloud_status_queue != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Failed to create Cloud GUI status queue"
    );

    ESP_LOGI(TAG, "Application GUI initialized");

    return ESP_OK;
}

esp_err_t app_gui_start_ui_task(void)
{
    if (s_wifi_status_queue == NULL) {
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

/* Demo API ----------------------------------------------------------------- */
esp_err_t app_gui_start_running_demo_task(void)
{
    if (s_wifi_status_queue == NULL) {
        ESP_LOGE(TAG, "Application GUI is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_demo_task_handle != NULL) {
        ESP_LOGW(TAG, "LVGL demo task is already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreate(
        app_gui_running_demo,
        "lvgl_demo",
        APP_GUI_DEMO_TASK_STACK_SIZE_BYTES,
        NULL,
        APP_GUI_TASK_PRIORITY,
        &s_demo_task_handle
    );

    if (task_ret != pdPASS) {
        s_demo_task_handle = NULL;
        ESP_LOGE(TAG,
                 "Failed to create LVGL demo task with %u-byte stack",
                 (unsigned int)APP_GUI_DEMO_TASK_STACK_SIZE_BYTES);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "LVGL demo task started with %u-byte stack",
             (unsigned int)APP_GUI_DEMO_TASK_STACK_SIZE_BYTES);

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

/* Screen State API --------------------------------------------------------- */
esp_err_t app_gui_set_screen_id(
    app_gui_screen_id_t screen_id)
{
    ESP_RETURN_ON_FALSE(
        (screen_id == APP_GUI_SCREEN_NONE) ||
        (screen_id == APP_GUI_SCREEN_WIFI) ||
        (screen_id == APP_GUI_SCREEN_SENSOR),
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid application screen ID: %d",
        (int)screen_id
    );

    taskENTER_CRITICAL(&s_screen_id_lock);
    s_current_screen_id = screen_id;
    taskEXIT_CRITICAL(&s_screen_id_lock);

    ESP_LOGD(TAG, "Application screen ID set to %d", (int)screen_id);

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

/* Screen Lifecycle API ----------------------------------------------------- */
esp_err_t app_gui_clear_screen(void)
{
    /*
     * This function intentionally does not acquire the LVGL mutex. Callers
     * must already serialize LVGL access as documented in app_gui.h.
     */
    lv_obj_t* m_currentScreen = lv_screen_active();

    ESP_RETURN_ON_FALSE(m_currentScreen != NULL,
    ESP_ERR_INVALID_RESPONSE,
    TAG,
    "Failed to get current screen");

    lv_obj_t* m_newScreen = lv_obj_create(NULL);
    ESP_RETURN_ON_FALSE(m_newScreen != NULL,
    ESP_ERR_INVALID_RESPONSE,
    TAG,
    "Failed to create new screent");

    lv_obj_set_size(m_newScreen, LCD_H_RES, LCD_V_RES);
    lv_obj_center(m_newScreen);

    lv_screen_load(m_newScreen);

    s_wifi_mode_label = NULL;
    s_wifi_ssid_label = NULL;
    s_wifi_ip_label = NULL;
    s_sensor_temperature_label = NULL;
    s_sensor_humidity_label = NULL;
    s_sensor_state_label = NULL;
    s_sensor_cloud_label = NULL;
    s_sensor_cloud_dot = NULL;
    (void)app_gui_set_screen_id(APP_GUI_SCREEN_NONE);

    lv_obj_del(m_currentScreen);
    return ESP_OK;
}
