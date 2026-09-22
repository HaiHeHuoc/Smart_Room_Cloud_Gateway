#pragma once

#include <stdint.h>

/**
 * @brief Recover the unsigned FATFS file size preserved by ESP-IDF FAT VFS.
 *
 * ESP-IDF 6.0.1 FAT VFS copies FAT32 `FILINFO.fsize` into a signed 32-bit
 * `struct stat::st_size`.  Files at or above 2 GiB therefore look negative
 * through the POSIX type even though FATFS supplied a valid unsigned DWORD.
 */
uint64_t sd_card_manager_fatfs_vfs_file_size_bytes(int32_t vfs_stat_size);
