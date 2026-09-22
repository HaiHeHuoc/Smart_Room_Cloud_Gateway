#include "sd_card_manager_usage.h"

bool sd_card_manager_usage_from_capacity(
    uint64_t total_bytes,
    uint64_t free_bytes,
    sd_card_manager_usage_values_t *usage)
{
    if ((usage == NULL) || (total_bytes == 0U) ||
        (free_bytes > total_bytes))
    {
        return false;
    }

    *usage = (sd_card_manager_usage_values_t){
        .total_bytes = total_bytes,
        .used_bytes = total_bytes - free_bytes,
        .free_bytes = free_bytes,
    };
    return true;
}
