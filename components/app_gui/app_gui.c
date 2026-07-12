/* Includes ----------------------------------------------------------------- */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ui_manager_lvgl.h"
#include "app_gui.h"
#include "esp_log.h"
#include "esp_err.h"
#include "lvgl.h"

/* Macros ------------------------------------------------------------------- */
#define APP_GUI_WIFI_STATUS_QUEUE_LENGTH 1U
#define APP_GUI_UI_TASK_STACK_SIZE_BYTES   (24U * 1024U)
#define APP_GUI_DEMO_TASK_STACK_SIZE_BYTES (4U * 1024U)
#define APP_GUI_TASK_PRIORITY              5U
#define APP_GUI_TASK_PERIOD_MS              33U
#define APP_GUI_STACK_LOG_PERIOD_MS         (60U * 1000U)
#define APP_GUI_STACK_WARNING_BYTES         (2U * 1024U)
#define APP_GUI_WIFI_VALUE_WIDTH_PX          100
#define APP_GUI_WIFI_VALUE_HEIGHT_PX          18
#define APP_GUI_WIFI_IP_VALUE_WIDTH_PX        120

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_GUI";

/* Static Variables --------------------------------------------------------- */
static QueueHandle_t s_wifi_status_queue = NULL;
static lv_obj_t *s_wifi_mode_label = NULL;
static lv_obj_t *s_wifi_ssid_label = NULL;
static lv_obj_t *s_wifi_ip_label = NULL;

/* Application -------------------------------------------------------------- */
/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void app_gui_create_demo_screen(void)
{
    // waiting for the LVGL mutex to ensure thread safety
    ui_manager_lvgl_wait_for_mutex();

    lv_obj_t *screen = lv_screen_active();

    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // Set background color to white

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "LVGL OK");
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_obj_center(label);

    ui_manager_lvgl_release_mutex();
}

/* Static Functions --------------------------------------------------------- */
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

    return label;
}

static esp_err_t app_gui_create_wifi_screen(void)
{
    lv_obj_t *screen = lv_screen_active();
    if (screen == NULL) {
        ESP_LOGE(TAG, "No active LVGL screen for Wi-Fi GUI");
        return ESP_ERR_INVALID_STATE;
    }

    lv_obj_clean(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,
                              lv_color_hex(0x101619),
                              LV_PART_MAIN);

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
        lv_obj_set_style_text_font(title,
                            &lv_font_montserrat_20,
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

    ESP_LOGI(TAG, "Wi-Fi status screen created");

    return ESP_OK;
}

static void app_gui_update_wifi_screen(const ui_wifi_status_t *status)
{
    if ((status == NULL) ||
        (s_wifi_mode_label == NULL) ||
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

    app_gui_update_wifi_screen(&wifi_status);

    ESP_LOGI(
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
        ESP_LOGI(TAG,
                 "%s minimum free stack: %u bytes",
                 task_name,
                 (unsigned int)minimum_free_stack);
    }
}

static void app_gui_process_lvgl(void)
{
    ui_manager_lvgl_wait_for_mutex();

    app_gui_process_wifi_status();
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

/**
 * @brief Demo function for LVGL while running
 * 
 * @param vPrama 
 */
static void app_gui_running_demo(void* vPrama)
{
    (void)vPrama;

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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // Set background color to white 
    lv_obj_t *label = lv_label_create(screen); 
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

    if (s_wifi_status_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi GUI status queue");
        return ESP_ERR_NO_MEM;
    }

    ui_manager_lvgl_wait_for_mutex();
    const esp_err_t screen_ret = app_gui_create_wifi_screen();
    ui_manager_lvgl_release_mutex();

    if (screen_ret != ESP_OK) {
        vQueueDelete(s_wifi_status_queue);
        s_wifi_status_queue = NULL;

        ESP_LOGE(TAG,
                 "Failed to create Wi-Fi GUI: %s",
                 esp_err_to_name(screen_ret));
        return screen_ret;
    }

    ESP_LOGI(TAG, "Application GUI initialized");

    return ESP_OK;
}

esp_err_t app_gui_start_ui_task(void)
{
    if (s_wifi_status_queue == NULL) {
        ESP_LOGE(TAG, "Application GUI is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreate(
        app_gui_ui_task,
        "app_gui_ui",
        APP_GUI_UI_TASK_STACK_SIZE_BYTES,
        NULL,
        APP_GUI_TASK_PRIORITY,
        NULL
    );

    if (task_ret != pdPASS) {
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

/**
 * @brief Start running demo task
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if app_gui is not
 *         initialized, or ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t app_gui_start_running_demo_task(void)
{
    if (s_wifi_status_queue == NULL) {
        ESP_LOGE(TAG, "Application GUI is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreate(
        app_gui_running_demo,
        "lvgl_demo",
        APP_GUI_DEMO_TASK_STACK_SIZE_BYTES,
        NULL,
        APP_GUI_TASK_PRIORITY,
        NULL
    );

    if (task_ret != pdPASS) {
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
