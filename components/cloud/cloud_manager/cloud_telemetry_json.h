#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cloud_manager.h"

/*
 * The prior 512-byte buffer had little headroom after adding two worst-case
 * 64-bit time values and the fixed 25-character ISO-8601 string. 640 bytes
 * keeps this bounded formatter conservative without material PSRAM cost.
 */
#define CLOUD_TELEMETRY_JSON_BUFFER_SIZE  640U

/**
 * @brief Validate the cloud-owned time representation before queueing it.
 *
 * A synchronized value must contain positive Unix timestamps and the fixed
 * ISO-8601 local format. An unsynchronized value is represented only as
 * false, zero, empty string, and zero. This also prevents unescaped input
 * from reaching the bounded JSON formatter.
 */
bool cloud_telemetry_json_is_valid_time_telemetry(
    const cloud_time_telemetry_t *time_telemetry);

/**
 * @brief Serialize one validated latest-value snapshot into caller storage.
 *
 * This helper performs no allocation, synchronization, or network work. It
 * preserves the existing flattened sensor schema and nested audio object,
 * adding the cloud-owned nested time object before the source field.
 */
esp_err_t cloud_telemetry_json_serialize(
    const cloud_sensor_telemetry_t *telemetry,
    char *buffer,
    size_t buffer_size,
    size_t *payload_length);
