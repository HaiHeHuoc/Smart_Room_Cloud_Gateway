#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

/*
 * Use only in components whose complete task call path has been audited for
 * external-stack safety. This deliberately overrides the dynamic task create
 * and matching delete APIs only inside the component that force-includes this
 * header; it is not a project-wide FreeRTOS policy.
 *
 * task.h/idf_additions.h are included before the macros so FreeRTOS/ESP-IDF
 * declarations are not rewritten by the component-local override.
 *
 * ESP-IDF requires a task created by xTaskCreateWithCaps() to be deleted with
 * vTaskDeleteWithCaps(). Keep both overrides together so rollback/error paths
 * cannot accidentally free a PSRAM-backed stack with vTaskDelete().
 */
#define xTaskCreate(task_code, task_name, stack_depth, parameter, priority, task_handle) \
    xTaskCreateWithCaps(                                                           \
        (task_code),                                                               \
        (task_name),                                                               \
        (stack_depth),                                                             \
        (parameter),                                                               \
        (priority),                                                                \
        (task_handle),                                                             \
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define vTaskDelete(task_handle) \
    vTaskDeleteWithCaps((task_handle))
