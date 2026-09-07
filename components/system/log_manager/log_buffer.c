/* Includes ----------------------------------------------------------------- */
#include "log_buffer.h"
#include <string.h>

/* Static Functions --------------------------------------------------------- */
static size_t length_at_head(const log_buffer_t *r)
{
    return (size_t)r->data[r->head] |
           ((size_t)r->data[(r->head + 1) % r->capacity] << 8);
}

/* Functions ---------------------------------------------------------------- */
void log_buffer_init(log_buffer_t *r, void *memory, size_t capacity)
{
    *r = (log_buffer_t){ .data = memory, .capacity = capacity };
}

void log_buffer_pop(log_buffer_t *r)
{
    if (!r->records) return;
    size_t n = length_at_head(r);
    r->head = (r->head + n + 2) % r->capacity;
    r->used -= n + 2;
    r->payload -= n;
    --r->records;
}

size_t log_buffer_push(log_buffer_t *r, const char *line, size_t n)
{
    if (n > UINT16_MAX || n + 2 > r->capacity) return 1;
    size_t dropped = 0;
    while (r->capacity - r->used < n + 2) {
        log_buffer_pop(r);
        ++dropped;
    }
    size_t tail = (r->head + r->used) % r->capacity;
    r->data[tail] = n & 255;
    r->data[(tail + 1) % r->capacity] = n >> 8;
    tail = (tail + 2) % r->capacity;
    size_t first = n < r->capacity - tail ? n : r->capacity - tail;
    memcpy(r->data + tail, line, first);
    memcpy(r->data, line + first, n - first);
    r->used += n + 2;
    r->payload += n;
    ++r->records;
    return dropped;
}

size_t log_buffer_peek(const log_buffer_t *r, char *line, size_t capacity)
{
    if (!r->records) return 0;
    size_t n = length_at_head(r);
    if (n + 1 > capacity) return 0;
    size_t start = (r->head + 2) % r->capacity;
    size_t first = n < r->capacity - start ? n : r->capacity - start;
    memcpy(line, r->data + start, first);
    memcpy(line + first, r->data, n - first);
    line[n] = 0;
    return n;
}

size_t log_line_finish(char *line, size_t capacity, bool truncated)
{
    if (capacity < 20) return 0;
    line[capacity - 2] = 0;
    size_t n = strlen(line);
    if (truncated) {
        static const char marker[] = " truncated=1";
        if (n > capacity - sizeof(marker) - 1) n = capacity - sizeof(marker) - 1;
        memcpy(line + n, marker, sizeof(marker) - 1);
        n += sizeof(marker) - 1;
    }
    for (size_t i = 0; i < n; ++i) {
        if ((unsigned char)line[i] < 32 || (unsigned char)line[i] == 127) line[i] = ' ';
    }
    line[n++] = '\n';
    line[n] = 0;
    return n;
}
