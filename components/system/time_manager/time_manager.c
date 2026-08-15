#include "time_manager.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

/* Configuration ------------------------------------------------------------ */

#define TIME_MANAGER_DEFAULT_SNTP_SERVER  "time.google.com"
#define TIME_MANAGER_DEFAULT_TIMEZONE     "ICT-7"

static const char *const TAG = "TIME_MANAGER";

/* Internal Types ----------------------------------------------------------- */

typedef struct
{
    bool initializing;
    bool initialized;
    bool start_in_progress;
    bool started;
    bool sntp_initialized;
    bool sntp_start_in_progress;
    bool sntp_service_started;

    char sntp_server[TIME_MANAGER_SNTP_SERVER_MAX_BYTES];
    char timezone[TIME_MANAGER_TIMEZONE_MAX_BYTES];

    time_manager_status_t status;
    time_manager_status_callback_t status_callback;
    void *status_callback_context;
} time_manager_context_t;

/* Static Variables --------------------------------------------------------- */

/*
 * Status and callback data are small, so use the same short critical-section
 * pattern as wifi_manager. It is safe from ESP-IDF's TCP/IP task and avoids
 * blocking the SNTP synchronization callback behind application code.
 */
static time_manager_context_t s_time_manager;
static portMUX_TYPE s_time_manager_lock =
    portMUX_INITIALIZER_UNLOCKED;

/* Function Prototypes ------------------------------------------------------ */

static esp_err_t time_manager_validate_string(
    const char *value,
    size_t capacity);
static void time_manager_copy_string(
    char *destination,
    size_t destination_size,
    const char *source);
static esp_err_t time_manager_apply_timezone(
    const char *timezone);
static void time_manager_notify_status_changed(void);
static void time_manager_record_start_failure(
    esp_err_t error);
static void time_manager_record_sync_failure(
    esp_err_t error);
static void time_manager_finish_sntp_start(
    esp_err_t result);
static void time_manager_sntp_sync_callback(
    struct timeval *sync_time);
static int64_t time_manager_days_from_civil(
    int32_t year,
    uint32_t month,
    uint32_t day);
static esp_err_t time_manager_get_utc_offset_seconds(
    const struct tm *local_time,
    const struct tm *utc_time,
    int64_t *offset_seconds);

/* Static Functions --------------------------------------------------------- */

static esp_err_t time_manager_validate_string(
    const char *value,
    size_t capacity)
{
    if ((value == NULL) ||
        (value[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return (strnlen(value, capacity) < capacity) ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void time_manager_copy_string(
    char *destination,
    size_t destination_size,
    const char *source)
{
    const size_t length = strnlen(source, destination_size - 1U);

    memcpy(destination, source, length);
    destination[length] = '\0';
}

static esp_err_t time_manager_apply_timezone(
    const char *timezone)
{
    if (setenv("TZ", timezone, 1) != 0)
    {
        const int saved_errno = errno;

        ESP_LOGE(
            TAG,
            "Failed to apply POSIX timezone (errno=%d)",
            saved_errno);

        return (saved_errno == ENOMEM) ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    tzset();

    return ESP_OK;
}

static void time_manager_notify_status_changed(void)
{
    time_manager_status_t status_snapshot = {0};
    time_manager_status_callback_t callback = NULL;
    void *callback_context = NULL;

    taskENTER_CRITICAL(&s_time_manager_lock);

    if (s_time_manager.initialized)
    {
        status_snapshot = s_time_manager.status;
        callback = s_time_manager.status_callback;
        callback_context = s_time_manager.status_callback_context;
    }

    taskEXIT_CRITICAL(&s_time_manager_lock);

    /* Application code always runs after the internal status lock is released. */
    if (callback != NULL)
    {
        callback(
            &status_snapshot,
            callback_context);
    }
}

static void time_manager_record_start_failure(
    esp_err_t error)
{
    bool status_updated = false;

    taskENTER_CRITICAL(&s_time_manager_lock);

    s_time_manager.start_in_progress = false;
    s_time_manager.started = false;
    s_time_manager.sntp_initialized = false;
    s_time_manager.sntp_start_in_progress = false;
    s_time_manager.sntp_service_started = false;

    if (s_time_manager.initialized)
    {
        s_time_manager.status.state = TIME_MANAGER_STATE_ERROR;
        s_time_manager.status.synced = false;
        s_time_manager.status.last_error = error;
        s_time_manager.status.network_available = false;
        status_updated = true;
    }

    taskEXIT_CRITICAL(&s_time_manager_lock);

    if (status_updated)
    {
        time_manager_notify_status_changed();
    }
}

static void time_manager_record_sync_failure(
    esp_err_t error)
{
    bool status_updated = false;

    taskENTER_CRITICAL(&s_time_manager_lock);

    if (s_time_manager.initialized &&
        s_time_manager.started &&
        s_time_manager.sntp_initialized)
    {
        s_time_manager.status.state =
            s_time_manager.status.network_available ?
                TIME_MANAGER_STATE_ERROR :
                TIME_MANAGER_STATE_WAITING_NETWORK;
        s_time_manager.status.last_error = error;
        status_updated = true;
    }

    taskEXIT_CRITICAL(&s_time_manager_lock);

    if (status_updated)
    {
        time_manager_notify_status_changed();
    }
}

static void time_manager_finish_sntp_start(
    esp_err_t result)
{
    bool status_updated = false;

    taskENTER_CRITICAL(&s_time_manager_lock);

    if (s_time_manager.initialized &&
        s_time_manager.started &&
        s_time_manager.sntp_initialized)
    {
        s_time_manager.sntp_start_in_progress = false;

        if (result == ESP_OK)
        {
            s_time_manager.sntp_service_started = true;

            if (!s_time_manager.status.network_available)
            {
                if (s_time_manager.status.state !=
                    TIME_MANAGER_STATE_WAITING_NETWORK)
                {
                    s_time_manager.status.state =
                        TIME_MANAGER_STATE_WAITING_NETWORK;
                    status_updated = true;
                }
            }
            else if (s_time_manager.status.state !=
                     TIME_MANAGER_STATE_SYNCED)
            {
                if ((s_time_manager.status.state !=
                     TIME_MANAGER_STATE_SYNCING) ||
                    (s_time_manager.status.last_error != ESP_OK))
                {
                    s_time_manager.status.state =
                        TIME_MANAGER_STATE_SYNCING;
                    s_time_manager.status.last_error = ESP_OK;
                    status_updated = true;
                }
            }
        }
        else
        {
            s_time_manager.sntp_service_started = false;
            s_time_manager.status.state =
                s_time_manager.status.network_available ?
                    TIME_MANAGER_STATE_ERROR :
                    TIME_MANAGER_STATE_WAITING_NETWORK;
            s_time_manager.status.last_error = result;
            status_updated = true;
        }
    }

    taskEXIT_CRITICAL(&s_time_manager_lock);

    if (status_updated)
    {
        time_manager_notify_status_changed();
    }
}

static void time_manager_sntp_sync_callback(
    struct timeval *sync_time)
{
    uint32_t sync_count = 0U;
    bool status_updated = false;
    const time_t synchronized_time = time(NULL);

    (void)sync_time;

    if (synchronized_time == (time_t)-1)
    {
        ESP_LOGE(TAG, "SNTP callback could not read system time");
        time_manager_record_sync_failure(ESP_FAIL);
        return;
    }

    taskENTER_CRITICAL(&s_time_manager_lock);

    if (s_time_manager.initialized &&
        s_time_manager.started &&
        s_time_manager.sntp_initialized)
    {
        s_time_manager.status.state =
            s_time_manager.status.network_available ?
                TIME_MANAGER_STATE_SYNCED :
                TIME_MANAGER_STATE_WAITING_NETWORK;
        s_time_manager.status.synced = true;
        s_time_manager.status.last_error = ESP_OK;
        s_time_manager.status.last_sync_unix = synchronized_time;
        s_time_manager.status.sync_count++;
        sync_count = s_time_manager.status.sync_count;
        status_updated = true;
    }

    taskEXIT_CRITICAL(&s_time_manager_lock);

    if (status_updated)
    {
        ESP_LOGI(
            TAG,
            "SNTP synchronized system time (count=%" PRIu32 ")",
            sync_count);

        time_manager_notify_status_changed();
    }
}

/*
 * Convert a civil date to a day count. The fixed epoch is irrelevant because
 * ISO-8601 offset calculation subtracts UTC and local values produced from
 * the same instant. This avoids relying on non-portable tm_gmtoff or %z.
 */
static int64_t time_manager_days_from_civil(
    int32_t year,
    uint32_t month,
    uint32_t day)
{
    year -= (month <= 2U) ? 1 : 0;

    const int32_t era =
        (year >= 0) ? (year / 400) : ((year - 399) / 400);
    const uint32_t year_of_era =
        (uint32_t)(year - (era * 400));
    const uint32_t month_from_march =
        (month > 2U) ? (month - 3U) : (month + 9U);
    const uint32_t day_of_year =
        ((153U * month_from_march) + 2U) / 5U + day - 1U;
    const uint32_t day_of_era =
        (year_of_era * 365U) + (year_of_era / 4U) -
        (year_of_era / 100U) + day_of_year;

    return ((int64_t)era * 146097LL) +
        (int64_t)day_of_era - 719468LL;
}

static esp_err_t time_manager_get_utc_offset_seconds(
    const struct tm *local_time,
    const struct tm *utc_time,
    int64_t *offset_seconds)
{
    if ((local_time == NULL) ||
        (utc_time == NULL) ||
        (offset_seconds == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t local_seconds =
        (time_manager_days_from_civil(
            local_time->tm_year + 1900,
            (uint32_t)local_time->tm_mon + 1U,
            (uint32_t)local_time->tm_mday) * 86400LL) +
        ((int64_t)local_time->tm_hour * 3600LL) +
        ((int64_t)local_time->tm_min * 60LL) +
        (int64_t)local_time->tm_sec;

    const int64_t utc_seconds =
        (time_manager_days_from_civil(
            utc_time->tm_year + 1900,
            (uint32_t)utc_time->tm_mon + 1U,
            (uint32_t)utc_time->tm_mday) * 86400LL) +
        ((int64_t)utc_time->tm_hour * 3600LL) +
        ((int64_t)utc_time->tm_min * 60LL) +
        (int64_t)utc_time->tm_sec;

    const int64_t offset = local_seconds - utc_seconds;

    /* A valid civil timezone offset is always strictly within one day. */
    if ((offset <= -86400LL) ||
        (offset >= 86400LL))
    {
        return ESP_FAIL;
    }

    *offset_seconds = offset;

    return ESP_OK;
}

/* Functions ---------------------------------------------------------------- */

time_manager_config_t time_manager_default_config(void)
{
    return (time_manager_config_t) {
        .sntp_server = TIME_MANAGER_DEFAULT_SNTP_SERVER,
        .timezone = TIME_MANAGER_DEFAULT_TIMEZONE,
    };
}

esp_err_t time_manager_init(
    const time_manager_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = time_manager_validate_string(
        config->sntp_server,
        TIME_MANAGER_SNTP_SERVER_MAX_BYTES);

    if (result != ESP_OK)
    {
        return result;
    }

    result = time_manager_validate_string(
        config->timezone,
        TIME_MANAGER_TIMEZONE_MAX_BYTES);

    if (result != ESP_OK)
    {
        return result;
    }

    taskENTER_CRITICAL(&s_time_manager_lock);

    if (s_time_manager.initialized ||
        s_time_manager.initializing)
    {
        taskEXIT_CRITICAL(&s_time_manager_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_time_manager.initializing = true;
    time_manager_copy_string(
        s_time_manager.sntp_server,
        sizeof(s_time_manager.sntp_server),
        config->sntp_server);
    time_manager_copy_string(
        s_time_manager.timezone,
        sizeof(s_time_manager.timezone),
        config->timezone);

    taskEXIT_CRITICAL(&s_time_manager_lock);

    const esp_err_t timezone_result =
        time_manager_apply_timezone(s_time_manager.timezone);

    if (timezone_result != ESP_OK)
    {
        taskENTER_CRITICAL(&s_time_manager_lock);
        memset(&s_time_manager, 0, sizeof(s_time_manager));
        taskEXIT_CRITICAL(&s_time_manager_lock);

        return timezone_result;
    }

    taskENTER_CRITICAL(&s_time_manager_lock);

    s_time_manager.status = (time_manager_status_t) {
        .state = TIME_MANAGER_STATE_INITIALIZED,
        .synced = false,
        .last_error = ESP_OK,
        .last_sync_unix = 0,
        .sync_count = 0U,
        .network_available = false,
    };
    s_time_manager.initialized = true;
    s_time_manager.initializing = false;

    taskEXIT_CRITICAL(&s_time_manager_lock);

    ESP_LOGI(
        TAG,
        "Initialized (server=%s, timezone=%s)",
        s_time_manager.sntp_server,
        s_time_manager.timezone);

    return ESP_OK;
}

esp_err_t time_manager_start(void)
{
    taskENTER_CRITICAL(&s_time_manager_lock);

    if (!s_time_manager.initialized ||
        s_time_manager.start_in_progress ||
        s_time_manager.started)
    {
        taskEXIT_CRITICAL(&s_time_manager_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_time_manager.start_in_progress = true;

    taskEXIT_CRITICAL(&s_time_manager_lock);

    esp_sntp_config_t sntp_config =
        /* lwIP retains this copied static string for later DNS resolution. */
        ESP_NETIF_SNTP_DEFAULT_CONFIG(s_time_manager.sntp_server);

    /* The application starts SNTP after it reports a valid IPv4 address. */
    sntp_config.wait_for_sync = false;
    sntp_config.start = false;
    sntp_config.sync_cb = time_manager_sntp_sync_callback;

    const esp_err_t result = esp_netif_sntp_init(&sntp_config);

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to configure ESP-NETIF SNTP: %s",
            esp_err_to_name(result));

        time_manager_record_start_failure(result);

        return result;
    }

    taskENTER_CRITICAL(&s_time_manager_lock);
    s_time_manager.start_in_progress = false;
    s_time_manager.started = true;
    s_time_manager.sntp_initialized = true;
    s_time_manager.sntp_start_in_progress = false;
    s_time_manager.sntp_service_started = false;
    s_time_manager.status.state = TIME_MANAGER_STATE_WAITING_NETWORK;
    s_time_manager.status.synced = false;
    s_time_manager.status.last_error = ESP_OK;
    s_time_manager.status.network_available = false;
    taskEXIT_CRITICAL(&s_time_manager_lock);

    ESP_LOGI(TAG, "ESP-NETIF SNTP configured; waiting for IPv4");

    time_manager_notify_status_changed();

    return ESP_OK;
}

esp_err_t time_manager_notify_network_state(
    bool has_ipv4_address)
{
    bool status_updated = false;
    bool start_sntp = false;

    taskENTER_CRITICAL(&s_time_manager_lock);

    if (!s_time_manager.initialized ||
        !s_time_manager.started ||
        !s_time_manager.sntp_initialized)
    {
        taskEXIT_CRITICAL(&s_time_manager_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const bool was_network_available =
        s_time_manager.status.network_available;

    if (!has_ipv4_address)
    {
        if (was_network_available)
        {
            s_time_manager.status.network_available = false;
            s_time_manager.status.state =
                TIME_MANAGER_STATE_WAITING_NETWORK;
            status_updated = true;
        }
    }
    else
    {
        s_time_manager.status.network_available = true;

        if (!s_time_manager.sntp_start_in_progress &&
            (!s_time_manager.sntp_service_started ||
             !was_network_available))
        {
            s_time_manager.sntp_start_in_progress = true;
            s_time_manager.status.state = TIME_MANAGER_STATE_SYNCING;
            s_time_manager.status.last_error = ESP_OK;
            start_sntp = true;
            status_updated = true;
        }
        else if (!was_network_available)
        {
            s_time_manager.status.state = TIME_MANAGER_STATE_SYNCING;
            s_time_manager.status.last_error = ESP_OK;
            status_updated = true;
        }
    }

    taskEXIT_CRITICAL(&s_time_manager_lock);

    if (status_updated)
    {
        time_manager_notify_status_changed();
    }

    if (!start_sntp)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting ESP-NETIF SNTP after IPv4 notification");

    const esp_err_t result = esp_netif_sntp_start();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start ESP-NETIF SNTP: %s",
            esp_err_to_name(result));
    }

    time_manager_finish_sntp_start(result);

    return result;
}

esp_err_t time_manager_register_status_callback(
    time_manager_status_callback_t callback,
    void *user_context)
{
    taskENTER_CRITICAL(&s_time_manager_lock);

    if (!s_time_manager.initialized)
    {
        taskEXIT_CRITICAL(&s_time_manager_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_time_manager.status_callback = callback;
    s_time_manager.status_callback_context =
        (callback != NULL) ? user_context : NULL;

    taskEXIT_CRITICAL(&s_time_manager_lock);

    ESP_LOGD(
        TAG,
        "Status callback %s",
        (callback != NULL) ? "registered" : "removed");

    return ESP_OK;
}

esp_err_t time_manager_get_status(
    time_manager_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_time_manager_lock);

    if (!s_time_manager.initialized)
    {
        taskEXIT_CRITICAL(&s_time_manager_lock);
        return ESP_ERR_INVALID_STATE;
    }

    *status = s_time_manager.status;

    taskEXIT_CRITICAL(&s_time_manager_lock);

    return ESP_OK;
}

bool time_manager_is_synced(void)
{
    bool synced = false;

    taskENTER_CRITICAL(&s_time_manager_lock);
    synced = s_time_manager.initialized &&
        s_time_manager.status.synced;
    taskEXIT_CRITICAL(&s_time_manager_lock);

    return synced;
}

esp_err_t time_manager_get_unix_time(
    time_t *unix_time)
{
    if (unix_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!time_manager_is_synced())
    {
        return ESP_ERR_INVALID_STATE;
    }

    const time_t current_time = time(NULL);

    if (current_time == (time_t)-1)
    {
        return ESP_FAIL;
    }

    *unix_time = current_time;

    return ESP_OK;
}

esp_err_t time_manager_get_local_time(
    struct tm *local_time)
{
    if (local_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    time_t current_time;
    const esp_err_t result =
        time_manager_get_unix_time(&current_time);

    if (result != ESP_OK)
    {
        return result;
    }

    if (localtime_r(&current_time, local_time) == NULL)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t time_manager_format_iso8601(
    char *buffer,
    size_t buffer_size)
{
    if (buffer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (buffer_size < TIME_MANAGER_ISO8601_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = '\0';

    time_t current_time;
    esp_err_t result = time_manager_get_unix_time(&current_time);

    if (result != ESP_OK)
    {
        return result;
    }

    struct tm local_time;
    struct tm utc_time;

    if ((localtime_r(&current_time, &local_time) == NULL) ||
        (gmtime_r(&current_time, &utc_time) == NULL))
    {
        return ESP_FAIL;
    }

    int64_t offset_seconds;
    result = time_manager_get_utc_offset_seconds(
        &local_time,
        &utc_time,
        &offset_seconds);

    if (result != ESP_OK)
    {
        return result;
    }

    const char offset_sign = (offset_seconds >= 0) ? '+' : '-';
    const uint64_t absolute_offset_seconds =
        (offset_seconds >= 0) ?
            (uint64_t)offset_seconds :
            (uint64_t)(-offset_seconds);
    const uint32_t offset_hours =
        (uint32_t)(absolute_offset_seconds / 3600U);
    const uint32_t offset_minutes =
        (uint32_t)((absolute_offset_seconds % 3600U) / 60U);

    const int written = snprintf(
        buffer,
        buffer_size,
        "%04d-%02d-%02dT%02d:%02d:%02d%c%02" PRIu32 ":%02" PRIu32,
        local_time.tm_year + 1900,
        local_time.tm_mon + 1,
        local_time.tm_mday,
        local_time.tm_hour,
        local_time.tm_min,
        local_time.tm_sec,
        offset_sign,
        offset_hours,
        offset_minutes);

    if (written < 0)
    {
        buffer[0] = '\0';
        return ESP_FAIL;
    }

    if ((size_t)written >= buffer_size)
    {
        buffer[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
