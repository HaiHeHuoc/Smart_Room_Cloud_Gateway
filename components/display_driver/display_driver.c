#include "include/display_driver.h"

#include "esp_log.h"
#include "esp_check.h"

const static char * TAG = "display_driver";

esp_err_t display_driver_init(display_driver_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");

    handle->io_handle = NULL;
    handle->panel_handle = NULL;

    /*
        Add your display driver initialization code here
            such as initializing the LCD panel and IO handles.
    */

    ESP_LOGI(TAG, "Display Driver initialized successfully");
    return ESP_OK;
}

esp_err_t display_driver_set_backlight(bool enable)
{
    ESP_LOGI(TAG, "Setting backlight: %s", enable ? "ON" : "OFF");

    /*
        Add your code to control the backlight of display
    */

    return ESP_OK;
}

esp_err_t display_driver_raw_color_test(const display_driver_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");

    ESP_LOGI(TAG, "Performing raw color test on the display");

    /*
        Add your code to perform the raw color test on the display
    */

    return ESP_OK;
}