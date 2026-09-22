#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Internal, copied capacity facts after FATFS geometry has been queried. */
typedef struct
{
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
} sd_card_manager_usage_values_t;

/**
 * Validate FATFS-derived capacity values and calculate used bytes without
 * unsigned underflow. A mounted FAT volume cannot have zero total capacity.
 */
bool sd_card_manager_usage_from_capacity(
    uint64_t total_bytes,
    uint64_t free_bytes,
    sd_card_manager_usage_values_t *usage);
