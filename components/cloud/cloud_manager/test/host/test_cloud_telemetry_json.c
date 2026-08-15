#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cloud_manager.h"
#include "cloud_telemetry_json.h"

static unsigned s_tests_run = 0U;
static unsigned s_tests_failed = 0U;

#define TEST_CHECK(condition) \
    do { \
        s_tests_run++; \
        if (!(condition)) { \
            s_tests_failed++; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        } \
    } while (0)

static cloud_sensor_telemetry_t canonical_telemetry(void)
{
    cloud_sensor_telemetry_t telemetry = {
        .temperature_c = 30.1F,
        .humidity_percent = 64.5F,
        .data_valid = true,
        .data_stale = false,
        .sensor_state = 3,
        .last_error = ESP_OK,
        .sample_uptime_ms = 123456,
        .audio = {
            .state = CLOUD_AUDIO_STATE_RECORDING,
            .last_error = ESP_OK,
        },
    };

    return telemetry;
}

static void test_synchronized_payload(void)
{
    cloud_sensor_telemetry_t telemetry = canonical_telemetry();
    telemetry.time = (cloud_time_telemetry_t) {
        .synced = true,
        .unix_time = (time_t)1786782000,
        .local_time = "2026-08-15T14:20:00+07:00",
        .last_sync_unix = (time_t)1786780800,
    };

    char payload[CLOUD_TELEMETRY_JSON_BUFFER_SIZE] = {0};
    size_t payload_length = 0U;

    const esp_err_t result = cloud_telemetry_json_serialize(
        &telemetry,
        payload,
        sizeof(payload),
        &payload_length);

    static const char expected[] =
        "{\"temperature_c\":30.1,\"humidity_percent\":64.5,"
        "\"sensor_valid\":true,\"sensor_stale\":false,"
        "\"sensor_state\":3,\"last_error\":0,"
        "\"sample_uptime_ms\":123456,\"audio\":{"
        "\"state\":\"recording\",\"recording\":true,"
        "\"playback\":false,\"last_error\":0},\"time\":{"
        "\"synced\":true,\"unix\":1786782000,"
        "\"local\":\"2026-08-15T14:20:00+07:00\","
        "\"last_sync_unix\":1786780800},"
        "\"source\":\"esp32_cloud_manager\"}";

    TEST_CHECK(result == ESP_OK);
    TEST_CHECK(payload_length == strlen(expected));
    TEST_CHECK(strcmp(payload, expected) == 0);
}

static void test_unsynchronized_payload(void)
{
    cloud_sensor_telemetry_t telemetry = canonical_telemetry();

    char payload[CLOUD_TELEMETRY_JSON_BUFFER_SIZE] = {0};
    size_t payload_length = 0U;

    const esp_err_t result = cloud_telemetry_json_serialize(
        &telemetry,
        payload,
        sizeof(payload),
        &payload_length);

    static const char expected[] =
        "{\"temperature_c\":30.1,\"humidity_percent\":64.5,"
        "\"sensor_valid\":true,\"sensor_stale\":false,"
        "\"sensor_state\":3,\"last_error\":0,"
        "\"sample_uptime_ms\":123456,\"audio\":{"
        "\"state\":\"recording\",\"recording\":true,"
        "\"playback\":false,\"last_error\":0},\"time\":{"
        "\"synced\":false,\"unix\":0,\"local\":\"\","
        "\"last_sync_unix\":0},\"source\":\"esp32_cloud_manager\"}";

    TEST_CHECK(result == ESP_OK);
    TEST_CHECK(payload_length == strlen(expected));
    TEST_CHECK(strcmp(payload, expected) == 0);
}

static void test_maximum_numeric_payload_fits(void)
{
    cloud_sensor_telemetry_t telemetry = canonical_telemetry();
    telemetry.temperature_c = FLT_MAX;
    telemetry.humidity_percent = -FLT_MAX;
    telemetry.sensor_state = INT32_MIN;
    telemetry.last_error = INT32_MIN;
    telemetry.sample_uptime_ms = INT64_MIN;
    telemetry.audio.state = CLOUD_AUDIO_STATE_UNAVAILABLE;
    telemetry.audio.last_error = INT32_MIN;
    telemetry.time = (cloud_time_telemetry_t) {
        .synced = true,
        .unix_time = (time_t)INT64_MAX,
        .local_time = "2099-12-31T23:59:59+07:00",
        .last_sync_unix = (time_t)INT64_MAX,
    };

    char payload[CLOUD_TELEMETRY_JSON_BUFFER_SIZE] = {0};
    size_t payload_length = 0U;

    const esp_err_t result = cloud_telemetry_json_serialize(
        &telemetry,
        payload,
        sizeof(payload),
        &payload_length);

    TEST_CHECK(result == ESP_OK);
    TEST_CHECK(payload_length < sizeof(payload));
    TEST_CHECK(payload[payload_length] == '\0');
}

int main(void)
{
    test_synchronized_payload();
    test_unsynchronized_payload();
    test_maximum_numeric_payload_fits();

    if (s_tests_failed != 0U) {
        printf("%u/%u cloud telemetry JSON tests failed\n",
               s_tests_failed,
               s_tests_run);
        return 1;
    }

    printf("%u cloud telemetry JSON tests passed\n", s_tests_run);
    return 0;
}
