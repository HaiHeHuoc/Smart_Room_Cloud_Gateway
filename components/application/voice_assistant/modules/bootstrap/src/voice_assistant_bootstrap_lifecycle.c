#include "voice_assistant_bootstrap_lifecycle.h"

void voice_assistant_bootstrap_lifecycle_init(
    voice_assistant_bootstrap_lifecycle_t *lifecycle,
    const voice_assistant_bootstrap_stage_t *stages,
    size_t stage_count)
{
    if (lifecycle == NULL) {
        return;
    }
    *lifecycle = (voice_assistant_bootstrap_lifecycle_t) {
        .stages = stages,
        .stage_count = stage_count,
        .safe_for_retry = true,
    };
}

esp_err_t voice_assistant_bootstrap_lifecycle_rollback(
    voice_assistant_bootstrap_lifecycle_t *lifecycle,
    void *context)
{
    if ((lifecycle == NULL) || ((lifecycle->stage_count > 0U) &&
                                (lifecycle->stages == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }

    while (lifecycle->attempted_count > 0U) {
        const size_t index = lifecycle->attempted_count - 1U;
        const voice_assistant_bootstrap_stage_t *const stage =
            &lifecycle->stages[index];
        if (stage->rollback == NULL) {
            /* This owner may still use every earlier dependency. Do not
             * deinitialize them below a failed reverse stage. */
            lifecycle->started = false;
            lifecycle->safe_for_retry = false;
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t ret = stage->rollback(context);
        if (ret != ESP_OK) {
            /* A timed-out task/ISR owner can still access earlier stages.
             * Keep those dependencies intact and fail closed. */
            lifecycle->started = false;
            lifecycle->safe_for_retry = false;
            return ret;
        }
        --lifecycle->attempted_count;
    }
    lifecycle->started = false;
    lifecycle->safe_for_retry = true;
    return ESP_OK;
}

esp_err_t voice_assistant_bootstrap_lifecycle_start(
    voice_assistant_bootstrap_lifecycle_t *lifecycle,
    void *context,
    esp_err_t *rollback_error)
{
    if (rollback_error != NULL) {
        *rollback_error = ESP_OK;
    }
    if ((lifecycle == NULL) || ((lifecycle->stage_count > 0U) &&
                                (lifecycle->stages == NULL)) ||
        lifecycle->started || !lifecycle->safe_for_retry) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t index = 0U; index < lifecycle->stage_count; ++index) {
        const voice_assistant_bootstrap_stage_t *const stage =
            &lifecycle->stages[index];
        if (stage->start == NULL) {
            lifecycle->attempted_count = index + 1U;
            const esp_err_t cleanup =
                voice_assistant_bootstrap_lifecycle_rollback(lifecycle, context);
            if (rollback_error != NULL) {
                *rollback_error = cleanup;
            }
            return ESP_ERR_INVALID_STATE;
        }
        /* A failed start may own a partial resource. Include it in rollback. */
        lifecycle->attempted_count = index + 1U;
        const esp_err_t ret = stage->start(context);
        if (ret != ESP_OK) {
            const esp_err_t cleanup =
                voice_assistant_bootstrap_lifecycle_rollback(lifecycle, context);
            if (rollback_error != NULL) {
                *rollback_error = cleanup;
            }
            return ret;
        }
    }
    lifecycle->started = true;
    lifecycle->safe_for_retry = false;
    return ESP_OK;
}
