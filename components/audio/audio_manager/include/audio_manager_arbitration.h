#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Logical clients allowed to request audio-manager resources.
 *
 * This enum is ownership metadata only. A client never receives an I2S handle,
 * DMA buffer, file handle, or raw hardware ownership from this contract.
 */
typedef enum {
    AUDIO_MANAGER_CLIENT_NONE = 0,
    AUDIO_MANAGER_CLIENT_SYSTEM,
    AUDIO_MANAGER_CLIENT_XIAOZHI,
    AUDIO_MANAGER_CLIENT_NOTIFICATION,
    AUDIO_MANAGER_CLIENT_ALARM,
    AUDIO_MANAGER_CLIENT_RECORDER,
    AUDIO_MANAGER_CLIENT_UI,
    AUDIO_MANAGER_CLIENT_TEST,
    AUDIO_MANAGER_CLIENT_COUNT,
} audio_manager_client_t;

/** Resource class arbitrated by audio_manager. */
typedef enum {
    AUDIO_MANAGER_RESOURCE_NONE = 0,
    AUDIO_MANAGER_RESOURCE_CAPTURE,
    AUDIO_MANAGER_RESOURCE_PLAYBACK,
} audio_manager_resource_t;

/**
 * @brief What to do when the requested resource is already owned.
 *
 * Phase 16-A only defines this intent. Queue/preemption behavior is implemented
 * in later Phase-16 checkpoints; current production audio APIs keep their
 * existing busy/ESP_ERR_INVALID_STATE behavior until explicitly migrated.
 */
typedef enum {
    AUDIO_MANAGER_BUSY_REJECT = 0,
    AUDIO_MANAGER_BUSY_QUEUE,
    AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY,
} audio_manager_busy_policy_t;

/** Priority range for arbitration requests. Higher numeric value wins. */
#define AUDIO_MANAGER_PRIORITY_MIN 0U
#define AUDIO_MANAGER_PRIORITY_MAX 255U

/** Recommended project defaults. Callers may override within 0..255. */
#define AUDIO_MANAGER_PRIORITY_BACKGROUND      20U
#define AUDIO_MANAGER_PRIORITY_UI              30U
#define AUDIO_MANAGER_PRIORITY_NOTIFICATION    50U
#define AUDIO_MANAGER_PRIORITY_XIAOZHI         70U
#define AUDIO_MANAGER_PRIORITY_SYSTEM          90U
#define AUDIO_MANAGER_PRIORITY_CRITICAL_ALARM 100U

/**
 * @brief Project-owned description of one future audio-resource request.
 *
 * `request_id` is caller/application correlation metadata and must be non-zero.
 * It does not grant ownership by itself and is not an I2S/session handle.
 *
 * `interruptible` describes whether an already-granted operation from this
 * request may later be cooperatively preempted by policy. Preemption must still
 * be performed by audio_manager; callers must never stop/reconfigure I2S
 * directly.
 */
typedef struct {
    uint32_t request_id;
    audio_manager_client_t client;
    audio_manager_resource_t resource;
    uint8_t priority;
    audio_manager_busy_policy_t busy_policy;
    bool interruptible;
} audio_manager_request_t;

/**
 * @brief Build one request with stable project defaults for a known client.
 *
 * request_id must be non-zero and resource must be CAPTURE or PLAYBACK.
 * The returned request is only metadata; it does not touch manager state.
 */
esp_err_t audio_manager_request_make_default(
    uint32_t request_id,
    audio_manager_client_t client,
    audio_manager_resource_t resource,
    audio_manager_request_t *request);

/** Validate a request's enum/range/correlation fields without side effects. */
esp_err_t audio_manager_request_validate(const audio_manager_request_t *request);

/** Stable diagnostics strings. */
const char *audio_manager_client_to_string(audio_manager_client_t client);
const char *audio_manager_resource_to_string(audio_manager_resource_t resource);
const char *audio_manager_busy_policy_to_string(audio_manager_busy_policy_t policy);

#ifdef __cplusplus
}
#endif
