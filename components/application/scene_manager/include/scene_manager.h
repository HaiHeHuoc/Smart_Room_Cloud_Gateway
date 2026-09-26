#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCENE_MANAGER_CATALOG_COUNT 4U
#define SCENE_MANAGER_ID_MAX_LEN 8U

typedef enum {
    SCENE_MANAGER_OUTCOME_SUCCESS = 0,
    SCENE_MANAGER_OUTCOME_PARTIAL,
    SCENE_MANAGER_OUTCOME_BUSY,
    SCENE_MANAGER_OUTCOME_REJECTED,
    SCENE_MANAGER_OUTCOME_UNAVAILABLE,
    SCENE_MANAGER_OUTCOME_FAILED,
    SCENE_MANAGER_OUTCOME_SKIPPED,
} scene_manager_outcome_t;

typedef struct {
    char id[SCENE_MANAGER_ID_MAX_LEN + 1U];
    char name[16U];
} scene_manager_descriptor_t;

typedef struct {
    uint8_t count;
    scene_manager_descriptor_t entries[SCENE_MANAGER_CATALOG_COUNT];
} scene_manager_catalog_t;

typedef struct {
    bool available;
    uint32_t generation;
    char last_requested_scene[SCENE_MANAGER_ID_MAX_LEN + 1U];
    char last_completed_scene[SCENE_MANAGER_ID_MAX_LEN + 1U];
    scene_manager_outcome_t outcome;
    scene_manager_outcome_t light_outcome;
    scene_manager_outcome_t audio_outcome;
    esp_err_t last_error;
} scene_manager_status_t;

/** Initialize the built-in scene orchestrator. No task or queue is created. */
esp_err_t scene_manager_init(void);

/** Copy the fixed, compile-time catalog in stable product order. */
esp_err_t scene_manager_get_catalog(scene_manager_catalog_t *catalog);

/** Copy the last completed orchestration result; this is not a current-scene claim. */
esp_err_t scene_manager_get_status(scene_manager_status_t *status);

/**
 * Apply one exact built-in ID synchronously from normal task context.
 *
 * Calls are serialized without a queue. A concurrent request returns a copied
 * BUSY result. A partial result never rolls back a completed owner step.
 */
esp_err_t scene_manager_apply(const char *scene_id, scene_manager_status_t *result);

const char *scene_manager_outcome_to_string(scene_manager_outcome_t outcome);

#ifdef __cplusplus
}
#endif
