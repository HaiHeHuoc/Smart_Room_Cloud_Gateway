#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_INTERNAL 0x01U
#define MALLOC_CAP_8BIT     0x02U

static inline void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    (void)capabilities;
    return malloc(size);
}

static inline void heap_caps_free(void *memory)
{
    free(memory);
}
