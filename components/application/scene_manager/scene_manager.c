#include "scene_manager.h"

#include <string.h>

#include "audio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "light_manager.h"
#include "voice_assistant_playback_control.h"

typedef struct {
    const char *id;
    const char *name;
    light_manager_state_t light;
    uint32_t volume_percent;
    bool stop_eligible_local_playback;
} scene_definition_t;

static const scene_definition_t s_scenes[SCENE_MANAGER_CATALOG_COUNT] = {
    { "focus", "Focus", { true, 255U, 244U, 229U, 70U, LIGHT_MANAGER_EFFECT_SOLID }, 60U, false },
    { "relax", "Relax", { true, 255U, 160U, 80U, 45U, LIGHT_MANAGER_EFFECT_BREATH }, 40U, false },
    { "night", "Night", { true, 255U, 35U, 10U, 8U, LIGHT_MANAGER_EFFECT_SOLID }, 20U, false },
    { "all_off", "All Off", { false, 0U, 0U, 0U, 0U, LIGHT_MANAGER_EFFECT_SOLID }, 0U, true },
};

static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static scene_manager_status_t s_status;

static void scene_copy_id(char destination[SCENE_MANAGER_ID_MAX_LEN + 1U],
                          const char *source)
{
    (void)strncpy(destination, source, SCENE_MANAGER_ID_MAX_LEN);
    destination[SCENE_MANAGER_ID_MAX_LEN] = '\0';
}

static const scene_definition_t *scene_find(const char *id)
{
    if (id == NULL) return NULL;
    for (size_t index = 0U; index < SCENE_MANAGER_CATALOG_COUNT; ++index) {
        if (strcmp(id, s_scenes[index].id) == 0) return &s_scenes[index];
    }
    return NULL;
}

static scene_manager_outcome_t scene_outcome_from_error(esp_err_t error)
{
    if (error == ESP_OK) return SCENE_MANAGER_OUTCOME_SUCCESS;
    if ((error == ESP_ERR_INVALID_STATE) || (error == ESP_ERR_NOT_SUPPORTED)) {
        return SCENE_MANAGER_OUTCOME_UNAVAILABLE;
    }
    if (error == ESP_ERR_TIMEOUT) return SCENE_MANAGER_OUTCOME_BUSY;
    return SCENE_MANAGER_OUTCOME_FAILED;
}

static scene_manager_outcome_t scene_apply_audio(const scene_definition_t *scene,
                                                  esp_err_t *last_error)
{
    if (!scene->stop_eligible_local_playback) {
        const esp_err_t error = audio_manager_set_playback_volume_percent(scene->volume_percent);
        if (error != ESP_OK) *last_error = error;
        return scene_outcome_from_error(error);
    }

    audio_manager_playback_status_t playback = {0};
    esp_err_t error = voice_assistant_playback_get_status(&playback);
    if (error != ESP_OK) {
        *last_error = error;
        return scene_outcome_from_error(error);
    }
    if (playback.source == AUDIO_MANAGER_PLAYBACK_SOURCE_NONE) {
        return SCENE_MANAGER_OUTCOME_SUCCESS;
    }
    if (playback.source == AUDIO_MANAGER_PLAYBACK_SOURCE_PCM16_STREAM) {
        return SCENE_MANAGER_OUTCOME_SKIPPED;
    }

    voice_assistant_playback_control_result_t control = {0};
    error = voice_assistant_playback_control(VOICE_ASSISTANT_PLAYBACK_ACTION_STOP, &control);
    if (error != ESP_OK) {
        *last_error = error;
        return scene_outcome_from_error(error);
    }
    if (!control.accepted) {
        if ((control.outcome == VOICE_ASSISTANT_PLAYBACK_OUTCOME_NON_RESUMABLE_SOURCE) ||
            (control.outcome == VOICE_ASSISTANT_PLAYBACK_OUTCOME_NO_CURRENT_SOURCE)) {
            return SCENE_MANAGER_OUTCOME_SKIPPED;
        }
        return SCENE_MANAGER_OUTCOME_REJECTED;
    }
    /* A PTT-owned turn records a deferred override without disrupting voice. */
    return control.physically_applied ? SCENE_MANAGER_OUTCOME_SUCCESS
                                      : SCENE_MANAGER_OUTCOME_SKIPPED;
}

static scene_manager_outcome_t scene_aggregate(scene_manager_outcome_t light,
                                               scene_manager_outcome_t audio)
{
    if ((light == SCENE_MANAGER_OUTCOME_SUCCESS) &&
        ((audio == SCENE_MANAGER_OUTCOME_SUCCESS) || (audio == SCENE_MANAGER_OUTCOME_SKIPPED))) {
        return SCENE_MANAGER_OUTCOME_SUCCESS;
    }
    /* A successful domain must not be hidden behind another domain's transient
     * BUSY/UNAVAILABLE result: callers need a structured partial outcome and
     * must not infer rollback. SKIPPED alone is not physical application. */
    if ((light == SCENE_MANAGER_OUTCOME_SUCCESS) ||
        (audio == SCENE_MANAGER_OUTCOME_SUCCESS)) return SCENE_MANAGER_OUTCOME_PARTIAL;
    if (light == audio) return light;
    return SCENE_MANAGER_OUTCOME_FAILED;
}

esp_err_t scene_manager_init(void)
{
    if (s_initialized) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    s_status = (scene_manager_status_t) {
        .available = true,
        .outcome = SCENE_MANAGER_OUTCOME_SUCCESS,
        .light_outcome = SCENE_MANAGER_OUTCOME_SUCCESS,
        .audio_outcome = SCENE_MANAGER_OUTCOME_SUCCESS,
        .last_error = ESP_OK,
    };
    s_initialized = true;
    return ESP_OK;
}

esp_err_t scene_manager_get_catalog(scene_manager_catalog_t *catalog)
{
    if (catalog == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    *catalog = (scene_manager_catalog_t){ .count = SCENE_MANAGER_CATALOG_COUNT };
    for (size_t index = 0U; index < SCENE_MANAGER_CATALOG_COUNT; ++index) {
        scene_copy_id(catalog->entries[index].id, s_scenes[index].id);
        (void)strncpy(catalog->entries[index].name, s_scenes[index].name,
                      sizeof(catalog->entries[index].name) - 1U);
    }
    return ESP_OK;
}

esp_err_t scene_manager_get_status(scene_manager_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50U)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *status = s_status;
    (void)xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t scene_manager_apply(const char *scene_id, scene_manager_status_t *result)
{
    const scene_definition_t *scene = scene_find(scene_id);
    if ((scene == NULL) || (result == NULL)) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, 0U) != pdTRUE) {
        *result = (scene_manager_status_t){
            .available = true,
            .outcome = SCENE_MANAGER_OUTCOME_BUSY,
            .light_outcome = SCENE_MANAGER_OUTCOME_BUSY,
            .audio_outcome = SCENE_MANAGER_OUTCOME_BUSY,
            .last_error = ESP_ERR_TIMEOUT,
        };
        return ESP_OK;
    }

    scene_manager_status_t next = s_status;
    next.generation++;
    scene_copy_id(next.last_requested_scene, scene->id);
    next.last_error = ESP_OK;

    light_manager_state_t requested = scene->light;
    esp_err_t light_error = ESP_OK;
    if (scene->stop_eligible_local_playback) {
        light_error = light_manager_get_state(&requested);
        if (light_error == ESP_OK) requested.power_on = false;
    }
    if (light_error == ESP_OK) light_error = light_manager_set_state(&requested);
    next.light_outcome = scene_outcome_from_error(light_error);
    if (light_error != ESP_OK) next.last_error = light_error;

    next.audio_outcome = scene_apply_audio(scene, &next.last_error);
    next.outcome = scene_aggregate(next.light_outcome, next.audio_outcome);
    if ((next.outcome == SCENE_MANAGER_OUTCOME_SUCCESS) ||
        (next.outcome == SCENE_MANAGER_OUTCOME_PARTIAL)) {
        scene_copy_id(next.last_completed_scene, scene->id);
    }
    s_status = next;
    *result = next;
    (void)xSemaphoreGive(s_mutex);
    return ESP_OK;
}

const char *scene_manager_outcome_to_string(scene_manager_outcome_t outcome)
{
    switch (outcome) {
        case SCENE_MANAGER_OUTCOME_SUCCESS: return "success";
        case SCENE_MANAGER_OUTCOME_PARTIAL: return "partial";
        case SCENE_MANAGER_OUTCOME_BUSY: return "busy";
        case SCENE_MANAGER_OUTCOME_REJECTED: return "rejected";
        case SCENE_MANAGER_OUTCOME_UNAVAILABLE: return "unavailable";
        case SCENE_MANAGER_OUTCOME_FAILED: return "failed";
        case SCENE_MANAGER_OUTCOME_SKIPPED: return "skipped";
        default: return "failed";
    }
}
