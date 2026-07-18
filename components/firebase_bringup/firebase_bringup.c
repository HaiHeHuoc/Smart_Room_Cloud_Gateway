/* Includes ----------------------------------------------------------------- */
#include "firebase_bringup.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

/* Macros ------------------------------------------------------------------- */

#define FIREBASE_TEST_URL                                                   \
    "https://esp32-smart-room-gateway-default-rtdb."                        \
    "asia-southeast1.firebasedatabase.app/"                                 \
    "devices/esp32s3-001/latest.json"

#define FIREBASE_HTTP_TIMEOUT_MS    15000

/* Constants ---------------------------------------------------------------- */

static const char *TAG = "FIREBASE_TEST";

/* Functions ---------------------------------------------------------------- */

esp_err_t firebase_bringup_put_test(void)
{
    /*
     * Use distinctive values so we can confirm that the data
     * came from ESP32 instead of the PowerShell test.
     */
    static const char payload[] =
        "{"
            "\"temperature_c\":31.4,"
            "\"humidity_percent\":60.5,"
            "\"sensor_valid\":true,"
            "\"sensor_stale\":false,"
            "\"source\":\"esp32_bringup\""
        "}";

    esp_http_client_config_t config =
    {
        .url = FIREBASE_TEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FIREBASE_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        esp_http_client_set_method(
            client,
            HTTP_METHOD_PUT);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to set HTTP method: %s",
            esp_err_to_name(err));

        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json");

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to set header: %s",
            esp_err_to_name(err));

        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_set_post_field(
        client,
        payload,
        strlen(payload));

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to set payload: %s",
            esp_err_to_name(err));

        esp_http_client_cleanup(client);
        return err;
    }

    ESP_LOGI(TAG, "Sending HTTPS PUT to Firebase");

    err = esp_http_client_perform(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTPS request failed: %s",
            esp_err_to_name(err));

        esp_http_client_cleanup(client);
        return err;
    }

    int status_code =
        esp_http_client_get_status_code(client);

    ESP_LOGI(
        TAG,
        "Firebase HTTP status: %d",
        status_code);

    esp_http_client_cleanup(client);

    if (status_code < 200 ||
        status_code >= 300)
    {
        ESP_LOGE(
            TAG,
            "Firebase rejected request: HTTP %d",
            status_code);

        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firebase PUT successful");

    return ESP_OK;
}
