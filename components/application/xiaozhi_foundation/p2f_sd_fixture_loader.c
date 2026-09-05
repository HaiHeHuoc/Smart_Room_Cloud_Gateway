/* Validation-only SD fixture loader for Xiaozhi Phase 12 P2-F. */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card_manager.h"
#include "sdkconfig.h"
#include "xiaozhi_foundation.h"

#if CONFIG_XIAOZHI_FOUNDATION_P2F_SD_FIXTURE

#define XIAOZHI_P2F_SD_FIXTURE_PATH "/sdcard/xiaozhi/p2f_fixture.xzf"
#define XIAOZHI_P2F_SD_FIXTURE_SIZE 6030U
#define XIAOZHI_P2F_SD_WAIT_TIMEOUT_MS 30000U
#define XIAOZHI_P2F_SD_WAIT_POLL_MS 250U

static const char *const TAG = "XZ_P2F_SD";
static bool s_fixture_loaded = false;

extern uint8_t xiaozhi_p2f_fixture_start[]
    asm("_binary_xiaozhi_p2f_fixture_start");
extern uint8_t xiaozhi_p2f_fixture_end[]
    asm("_binary_xiaozhi_p2f_fixture_end");

extern esp_err_t __real_xiaozhi_foundation_request_transport_validation(
    xiaozhi_foundation_transport_t requested);

static uint16_t p2f_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t p2f_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static esp_err_t p2f_validate_loaded_header(const uint8_t *data, size_t size)
{
    if ((data == NULL) || (size != XIAOZHI_P2F_SD_FIXTURE_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if ((memcmp(data, "XZF1", 4U) != 0) ||
        (data[4] != 1U) ||
        (data[5] != 1U) ||
        (p2f_read_le16(&data[6]) != 24U) ||
        (p2f_read_le32(&data[8]) != 16000U) ||
        (data[12] != 1U) ||
        (data[13] != 0U) ||
        (p2f_read_le16(&data[14]) != 60U) ||
        (p2f_read_le32(&data[16]) != 33U) ||
        (p2f_read_le32(&data[20]) != (XIAOZHI_P2F_SD_FIXTURE_SIZE - 24U))) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t p2f_wait_for_sd_ready(void)
{
    const int64_t deadline_us = esp_timer_get_time() +
        ((int64_t)XIAOZHI_P2F_SD_WAIT_TIMEOUT_MS * 1000LL);

    while (!sd_card_manager_is_mounted()) {
        if (esp_timer_get_time() >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_P2F_SD_WAIT_POLL_MS));
    }

    return ESP_OK;
}

static esp_err_t p2f_load_fixture_from_sd(void)
{
    if (s_fixture_loaded) {
        return ESP_OK;
    }

    const size_t buffer_size =
        (size_t)(xiaozhi_p2f_fixture_end - xiaozhi_p2f_fixture_start);
    if (buffer_size != XIAOZHI_P2F_SD_FIXTURE_SIZE) {
        ESP_LOGE(TAG, "P2F_SD_FIXTURE result=FAIL reason=buffer-size size=%u",
                 (unsigned)buffer_size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = p2f_wait_for_sd_ready();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "P2F_SD_FIXTURE result=FAIL reason=sd-not-ready path=%s error=%s",
                 XIAOZHI_P2F_SD_FIXTURE_PATH,
                 esp_err_to_name(ret));
        return ret;
    }

    ret = sd_card_manager_acquire();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "P2F_SD_FIXTURE result=FAIL reason=lease path=%s error=%s",
                 XIAOZHI_P2F_SD_FIXTURE_PATH,
                 esp_err_to_name(ret));
        return ret;
    }

    FILE *file = fopen(XIAOZHI_P2F_SD_FIXTURE_PATH, "rb");
    if (file == NULL) {
        sd_card_manager_release();
        ESP_LOGE(TAG,
                 "P2F_SD_FIXTURE result=FAIL reason=open path=%s",
                 XIAOZHI_P2F_SD_FIXTURE_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    ret = ESP_OK;
    if ((fseek(file, 0L, SEEK_END) != 0) ||
        (ftell(file) != (long)XIAOZHI_P2F_SD_FIXTURE_SIZE) ||
        (fseek(file, 0L, SEEK_SET) != 0)) {
        ret = ESP_ERR_INVALID_SIZE;
    } else if (fread(xiaozhi_p2f_fixture_start,
                     1U,
                     XIAOZHI_P2F_SD_FIXTURE_SIZE,
                     file) != XIAOZHI_P2F_SD_FIXTURE_SIZE) {
        ret = ESP_FAIL;
    }

    if (fclose(file) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    sd_card_manager_release();

    if (ret == ESP_OK) {
        ret = p2f_validate_loaded_header(
            xiaozhi_p2f_fixture_start,
            XIAOZHI_P2F_SD_FIXTURE_SIZE);
    }

    if (ret != ESP_OK) {
        memset(xiaozhi_p2f_fixture_start, 0, XIAOZHI_P2F_SD_FIXTURE_SIZE);
        ESP_LOGE(TAG,
                 "P2F_SD_FIXTURE result=FAIL reason=content path=%s error=%s",
                 XIAOZHI_P2F_SD_FIXTURE_PATH,
                 esp_err_to_name(ret));
        return ret;
    }

    s_fixture_loaded = true;
    ESP_LOGI(TAG,
             "P2F_SD_FIXTURE result=READY path=%s bytes=%u frames=33 frame_ms=60 sample_rate=16000 channels=1",
             XIAOZHI_P2F_SD_FIXTURE_PATH,
             (unsigned)XIAOZHI_P2F_SD_FIXTURE_SIZE);
    return ESP_OK;
}

esp_err_t __wrap_xiaozhi_foundation_request_transport_validation(
    xiaozhi_foundation_transport_t requested)
{
#if CONFIG_XIAOZHI_FOUNDATION_P2F_E2E_ONLINE_VALIDATION
    const esp_err_t ret = p2f_load_fixture_from_sd();
    if (ret != ESP_OK) {
        return ret;
    }
#endif

    return __real_xiaozhi_foundation_request_transport_validation(requested);
}

#endif /* CONFIG_XIAOZHI_FOUNDATION_P2F_SD_FIXTURE */
