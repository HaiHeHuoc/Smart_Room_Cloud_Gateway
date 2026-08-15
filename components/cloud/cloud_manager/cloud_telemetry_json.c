/* Includes ----------------------------------------------------------------- */
#include "cloud_telemetry_json.h"

#include <inttypes.h>
#include <stdio.h>

/* Static Functions --------------------------------------------------------- */
static size_t cloud_telemetry_json_bounded_length(
    const char *text,
    size_t maximum_length)
{
    size_t length = 0U;

    if (text == NULL) {
        return maximum_length;
    }

    while ((length < maximum_length) &&
           (text[length] != '\0')) {
        ++length;
    }

    return length;
}

static bool cloud_telemetry_json_is_local_iso8601(
    const char *local_time)
{
    const size_t expected_length =
        CLOUD_TIME_LOCAL_ISO8601_BUFFER_SIZE - 1U;

    if (cloud_telemetry_json_bounded_length(
            local_time,
            CLOUD_TIME_LOCAL_ISO8601_BUFFER_SIZE) != expected_length) {
        return false;
    }

    if ((local_time[4] != '-') ||
        (local_time[7] != '-') ||
        (local_time[10] != 'T') ||
        (local_time[13] != ':') ||
        (local_time[16] != ':') ||
        ((local_time[19] != '+') &&
         (local_time[19] != '-')) ||
        (local_time[22] != ':')) {
        return false;
    }

    for (size_t index = 0U; index < expected_length; ++index) {
        if ((index == 4U) ||
            (index == 7U) ||
            (index == 10U) ||
            (index == 13U) ||
            (index == 16U) ||
            (index == 19U) ||
            (index == 22U)) {
            continue;
        }

        if ((local_time[index] < '0') ||
            (local_time[index] > '9')) {
            return false;
        }
    }

    return true;
}

static const char *cloud_telemetry_json_audio_state_to_string(
    cloud_audio_state_t state)
{
    switch (state) {
        case CLOUD_AUDIO_STATE_READY:
            return "ready";

        case CLOUD_AUDIO_STATE_IDLE:
            return "idle";

        case CLOUD_AUDIO_STATE_RECORDING:
            return "recording";

        case CLOUD_AUDIO_STATE_PROCESSING:
            return "processing";

        case CLOUD_AUDIO_STATE_PLAYBACK:
            return "playback";

        case CLOUD_AUDIO_STATE_ERROR:
            return "error";

        case CLOUD_AUDIO_STATE_UNAVAILABLE:
        default:
            return "unavailable";
    }
}

/* Functions ---------------------------------------------------------------- */
bool cloud_telemetry_json_is_valid_time_telemetry(
    const cloud_time_telemetry_t *time_telemetry)
{
    if (time_telemetry == NULL) {
        return false;
    }

    if (!time_telemetry->synced) {
        return (time_telemetry->unix_time == (time_t)0) &&
               (time_telemetry->local_time[0] == '\0') &&
               (time_telemetry->last_sync_unix == (time_t)0);
    }

    return (time_telemetry->unix_time > (time_t)0) &&
           (time_telemetry->last_sync_unix > (time_t)0) &&
           cloud_telemetry_json_is_local_iso8601(
               time_telemetry->local_time);
}

esp_err_t cloud_telemetry_json_serialize(
    const cloud_sensor_telemetry_t *telemetry,
    char *buffer,
    size_t buffer_size,
    size_t *payload_length)
{
    if ((telemetry == NULL) ||
        (buffer == NULL) ||
        (buffer_size == 0U) ||
        (payload_length == NULL) ||
        !cloud_telemetry_json_is_valid_time_telemetry(
            &telemetry->time)) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool audio_recording =
        telemetry->audio.state ==
        CLOUD_AUDIO_STATE_RECORDING;
    const bool audio_playback =
        telemetry->audio.state ==
        CLOUD_AUDIO_STATE_PLAYBACK;

    const int written =
        snprintf(
            buffer,
            buffer_size,
            "{"
                "\"temperature_c\":%.1f,"
                "\"humidity_percent\":%.1f,"
                "\"sensor_valid\":%s,"
                "\"sensor_stale\":%s,"
                "\"sensor_state\":%ld,"
                "\"last_error\":%ld,"
                "\"sample_uptime_ms\":%lld,"
                "\"audio\":{"
                    "\"state\":\"%s\","
                    "\"recording\":%s,"
                    "\"playback\":%s,"
                    "\"last_error\":%ld"
                "},"
                "\"time\":{"
                    "\"synced\":%s,"
                    "\"unix\":%" PRIdMAX ","
                    "\"local\":\"%s\","
                    "\"last_sync_unix\":%" PRIdMAX
                "},"
                "\"source\":\"esp32_cloud_manager\""
            "}",
            telemetry->temperature_c,
            telemetry->humidity_percent,
            telemetry->data_valid ? "true" : "false",
            telemetry->data_stale ? "true" : "false",
            (long)telemetry->sensor_state,
            (long)telemetry->last_error,
            (long long)telemetry->sample_uptime_ms,
            cloud_telemetry_json_audio_state_to_string(
                telemetry->audio.state),
            audio_recording ? "true" : "false",
            audio_playback ? "true" : "false",
            (long)telemetry->audio.last_error,
            telemetry->time.synced ? "true" : "false",
            (intmax_t)telemetry->time.unix_time,
            telemetry->time.local_time,
            (intmax_t)telemetry->time.last_sync_unix);

    if (written < 0) {
        buffer[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= buffer_size) {
        buffer[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    *payload_length = (size_t)written;

    return ESP_OK;
}
