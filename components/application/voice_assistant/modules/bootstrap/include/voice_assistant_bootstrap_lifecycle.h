#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** A component-owned bootstrap stage with an idempotent reverse operation. */
typedef struct {
    const char *name;
    esp_err_t (*start)(void *context);
    esp_err_t (*rollback)(void *context);
} voice_assistant_bootstrap_stage_t;

typedef struct {
    const voice_assistant_bootstrap_stage_t *stages;
    size_t stage_count;
    size_t attempted_count;
    bool started;
    bool safe_for_retry;
} voice_assistant_bootstrap_lifecycle_t;

/** Initialize a transaction. All stage callbacks must be component-owned. */
void voice_assistant_bootstrap_lifecycle_init(
    voice_assistant_bootstrap_lifecycle_t *lifecycle,
    const voice_assistant_bootstrap_stage_t *stages,
    size_t stage_count);

/** Start stages in order, rolling every attempted stage back on first failure. */
esp_err_t voice_assistant_bootstrap_lifecycle_start(
    voice_assistant_bootstrap_lifecycle_t *lifecycle,
    void *context,
    esp_err_t *rollback_error);

/**
 * Roll stages back in reverse order.
 *
 * Stops at the first rollback failure: an owner that did not stop can still
 * access earlier dependencies, so those dependencies must remain allocated.
 * A failure makes retry unsafe.
 */
esp_err_t voice_assistant_bootstrap_lifecycle_rollback(
    voice_assistant_bootstrap_lifecycle_t *lifecycle,
    void *context);
