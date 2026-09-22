#include "sd_card_manager_file_size.h"

uint64_t sd_card_manager_fatfs_vfs_file_size_bytes(int32_t vfs_stat_size)
{
    return (uint64_t)(uint32_t)vfs_stat_size;
}
