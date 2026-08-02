#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

/*
 * Use only in components whose complete task call path has been audited for
 * external-stack safety. This deliberately overrides xTaskCreate only inside
 * the component that force-includes this header; it is not a project-wide
 * FreeRTOS policy.
 *
 * task.h is included before the macro so FreeRTOS declarations are not
 * rewritten by the component-local override.
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
