#include <stdint.h>
#include <stdio.h>

#include "sd_card_manager_usage.h"
#include "sd_card_manager_file_size.h"

static int expect_usage(
    uint64_t total_bytes,
    uint64_t free_bytes,
    uint64_t expected_used_bytes)
{
    sd_card_manager_usage_values_t usage = {0};
    if (!sd_card_manager_usage_from_capacity(
            total_bytes, free_bytes, &usage) ||
        (usage.total_bytes != total_bytes) ||
        (usage.free_bytes != free_bytes) ||
        (usage.used_bytes != expected_used_bytes))
    {
        fprintf(stderr, "capacity result mismatch\n");
        return 1;
    }
    return 0;
}

static int expect_rejected(uint64_t total_bytes, uint64_t free_bytes)
{
    sd_card_manager_usage_values_t usage = {0};
    if (sd_card_manager_usage_from_capacity(total_bytes, free_bytes, &usage))
    {
        fprintf(stderr, "invalid capacity accepted\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;

    /* 32,768 FAT clusters, 64 sectors/cluster, 512-byte sectors. */
    const uint64_t cluster_size = 64ULL * 512ULL;
    const uint64_t total_bytes = 32768ULL * cluster_size;
    const uint64_t free_bytes = 8192ULL * cluster_size;
    failures += expect_usage(total_bytes, free_bytes, total_bytes - free_bytes);
    failures += expect_usage(total_bytes, 0U, total_bytes);
    failures += expect_usage(
        4294967295ULL * 4096ULL,
        2147483647ULL * 4096ULL,
        2147483648ULL * 4096ULL);
    failures += expect_rejected(0U, 0U);
    failures += expect_rejected(1024U, 1025U);

    failures += sd_card_manager_fatfs_vfs_file_size_bytes(2147483647) !=
                2147483647ULL;
    failures += sd_card_manager_fatfs_vfs_file_size_bytes((int32_t)0x80000000U) !=
                2147483648ULL;
    failures += sd_card_manager_fatfs_vfs_file_size_bytes((int32_t)0xFFFFFFFFU) !=
                4294967295ULL;
    return failures == 0 ? 0 : 1;
}
