#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    uint8_t *storage;
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t read_index;
    UBaseType_t write_index;
    UBaseType_t count;
} StaticQueue_t;

typedef StaticQueue_t *QueueHandle_t;

static inline QueueHandle_t xQueueCreateStatic(
    UBaseType_t length,
    UBaseType_t item_size,
    uint8_t *storage,
    StaticQueue_t *control)
{
    if ((length == 0U) || (item_size == 0U) || (storage == NULL) ||
        (control == NULL)) {
        return NULL;
    }
    *control = (StaticQueue_t) {
        .storage = storage,
        .length = length,
        .item_size = item_size,
    };
    return control;
}

static inline BaseType_t xQueueReset(QueueHandle_t queue)
{
    if (queue == NULL) {
        return pdFALSE;
    }
    queue->read_index = 0U;
    queue->write_index = 0U;
    queue->count = 0U;
    return pdTRUE;
}

static inline BaseType_t xQueueSend(
    QueueHandle_t queue,
    const void *item,
    uint32_t wait_ticks)
{
    (void)wait_ticks;
    if ((queue == NULL) || (item == NULL) || (queue->count == queue->length)) {
        return pdFALSE;
    }
    memcpy(queue->storage + ((size_t)queue->write_index * queue->item_size),
           item,
           queue->item_size);
    queue->write_index = (queue->write_index + 1U) % queue->length;
    ++queue->count;
    return pdTRUE;
}

static inline BaseType_t xQueueReceive(
    QueueHandle_t queue,
    void *item,
    uint32_t wait_ticks)
{
    (void)wait_ticks;
    if ((queue == NULL) || (item == NULL) || (queue->count == 0U)) {
        return pdFALSE;
    }
    memcpy(item,
           queue->storage + ((size_t)queue->read_index * queue->item_size),
           queue->item_size);
    queue->read_index = (queue->read_index + 1U) % queue->length;
    --queue->count;
    return pdTRUE;
}

static inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    return (queue == NULL) ? 0U : queue->count;
}
