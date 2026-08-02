/* Includes ----------------------------------------------------------------- */
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

/* Macros ------------------------------------------------------------------ */
#define LVGL_PSRAM_CAPABILITIES \
    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* Constants --------------------------------------------------------------- */
static const char *const TAG = "LVGL_PSRAM_ALLOC";

/* Static Variables -------------------------------------------------------- */
static bool s_first_allocation_verified = false;

/* Compile-Time Validation ------------------------------------------------- */
#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "LVGL PSRAM allocator requires the custom malloc backend"
#endif

/* Functions -------------------------------------------------------------- */
void lv_mem_init(void)
{
    s_first_allocation_verified = false;

    ESP_LOGI(
        TAG,
        "Custom LVGL allocator initialized: free=%u, largest=%u",
        (unsigned int)heap_caps_get_free_size(
            LVGL_PSRAM_CAPABILITIES),
        (unsigned int)heap_caps_get_largest_free_block(
            LVGL_PSRAM_CAPABILITIES));
}

void lv_mem_deinit(void)
{
    s_first_allocation_verified = false;
}

lv_mem_pool_t lv_mem_add_pool(
    void *memory,
    size_t bytes)
{
    /*
     * Additional LVGL-owned pools are not supported by this backend because
     * all allocations are delegated directly to the ESP-IDF PSRAM heap.
     */
    (void)memory;
    (void)bytes;

    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    if (size == 0U)
    {
        return NULL;
    }

    void *pointer =
        heap_caps_malloc(
            size,
            LVGL_PSRAM_CAPABILITIES);

    if (pointer == NULL)
    {
        ESP_LOGE(
            TAG,
            "LVGL PSRAM allocation failed: requested=%u, free=%u, largest=%u",
            (unsigned int)size,
            (unsigned int)heap_caps_get_free_size(
                LVGL_PSRAM_CAPABILITIES),
            (unsigned int)heap_caps_get_largest_free_block(
                LVGL_PSRAM_CAPABILITIES));

        return NULL;
    }

    if (!esp_ptr_external_ram(pointer))
    {
        ESP_LOGE(
            TAG,
            "LVGL allocation is not in external RAM: pointer=%p, size=%u",
            pointer,
            (unsigned int)size);

        heap_caps_free(pointer);
        return NULL;
    }

    if (!s_first_allocation_verified)
    {
        s_first_allocation_verified = true;

        ESP_LOGI(
            TAG,
            "LVGL heap verified in PSRAM: pointer=%p, first_size=%u",
            pointer,
            (unsigned int)size);
    }

    return pointer;
}

void *lv_realloc_core(
    void *pointer,
    size_t new_size)
{
    if (new_size == 0U)
    {
        heap_caps_free(pointer);
        return NULL;
    }

    void *new_pointer =
        heap_caps_realloc(
            pointer,
            new_size,
            LVGL_PSRAM_CAPABILITIES);

    if (new_pointer == NULL)
    {
        ESP_LOGE(
            TAG,
            "LVGL PSRAM reallocation failed: requested=%u, free=%u, largest=%u",
            (unsigned int)new_size,
            (unsigned int)heap_caps_get_free_size(
                LVGL_PSRAM_CAPABILITIES),
            (unsigned int)heap_caps_get_largest_free_block(
                LVGL_PSRAM_CAPABILITIES));

        return NULL;
    }

    if (!esp_ptr_external_ram(new_pointer))
    {
        ESP_LOGE(
            TAG,
            "LVGL reallocation is not in external RAM: pointer=%p, size=%u",
            new_pointer,
            (unsigned int)new_size);

        heap_caps_free(new_pointer);
        return NULL;
    }

    return new_pointer;
}

void lv_free_core(void *pointer)
{
    heap_caps_free(pointer);
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor)
{
    if (monitor == NULL)
    {
        return;
    }

    multi_heap_info_t heap_info = {0};

    heap_caps_get_info(
        &heap_info,
        LVGL_PSRAM_CAPABILITIES);

    const size_t total_size =
        heap_info.total_allocated_bytes +
        heap_info.total_free_bytes;

    monitor->total_size = total_size;
    monitor->free_cnt = heap_info.free_blocks;
    monitor->free_size = heap_info.total_free_bytes;
    monitor->free_biggest_size = heap_info.largest_free_block;
    monitor->used_cnt = heap_info.allocated_blocks;
    monitor->max_used =
        (total_size >= heap_info.minimum_free_bytes)
            ? total_size - heap_info.minimum_free_bytes
            : heap_info.total_allocated_bytes;
    monitor->used_pct =
        (total_size > 0U)
            ? (uint8_t)(
                (heap_info.total_allocated_bytes * 100U) /
                total_size)
            : 0U;
    monitor->frag_pct =
        (heap_info.total_free_bytes > 0U)
            ? (uint8_t)(
                100U -
                ((heap_info.largest_free_block * 100U) /
                 heap_info.total_free_bytes))
            : 0U;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(
               LVGL_PSRAM_CAPABILITIES,
               false)
               ? LV_RESULT_OK
               : LV_RESULT_INVALID;
}
