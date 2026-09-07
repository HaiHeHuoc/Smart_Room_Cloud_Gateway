#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private byte ring. Caller serializes all accesses; two-byte length framing. */
typedef struct {
    uint8_t *data;
    size_t capacity, head, used, payload, records;
} log_buffer_t;
void log_buffer_init(log_buffer_t *ring, void *memory, size_t capacity);
size_t log_buffer_push(log_buffer_t *ring, const char *line, size_t length);
size_t log_buffer_peek(const log_buffer_t *ring, char *line, size_t capacity);
void log_buffer_pop(log_buffer_t *ring);
/* Replace controls with spaces, guarantee one newline and NUL; mark truncation. */
size_t log_line_finish(char *line, size_t capacity, bool truncated);
