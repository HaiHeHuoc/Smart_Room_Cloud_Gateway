#include "voice_assistant_playback_control.h"

#include "voice_assistant_playback_turn_policy.h"

#include "app_log.h"
#include "audio_manager_named_playback.h"
#include "audio_manager_playback_arbiter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define VOICE_PLAYBACK_PAUSE_WAIT_MS 2500U
#define VOICE_PLAYBACK_POLL_MS       20U

static const char *const TAG = "VOICE_PB_POLICY";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized = false;
/* Catalog submission and PTT admission run from different task contexts.
 * Keep the short admission window explicit so a WAV cannot be accepted after
 * PTT has already decided that capture owns the speaker. */
static bool s_ptt_preparing = false;
static uint32_t s_catalog_submissions_inflight = 0U;
static voice_playback_turn_policy_t s_turn = {0};
static uint32_t s_catalog_request_id = 0x18220000U;

static bool playback_source_is_local(audio_manager_playback_source_t source)
{
    return (source == AUDIO_MANAGER_PLAYBACK_SOURCE_WAV) ||
           (source == AUDIO_MANAGER_PLAYBACK_SOURCE_RECORDED);
}

static voice_assistant_playback_outcome_t playback_outcome_from_error(
    esp_err_t error)
{
    if (error == ESP_ERR_NOT_SUPPORTED) {
        return VOICE_ASSISTANT_PLAYBACK_OUTCOME_NON_RESUMABLE_SOURCE;
    }
    if (error == ESP_ERR_INVALID_STATE) {
        return VOICE_ASSISTANT_PLAYBACK_OUTCOME_INVALID_STATE;
    }
    return VOICE_ASSISTANT_PLAYBACK_OUTCOME_CONTROL_FAILED;
}

static bool playback_action_to_turn_action(
    voice_assistant_playback_action_t action,
    voice_playback_turn_action_t *turn_action)
{
    if (turn_action == NULL) {
        return false;
    }
    switch (action) {
        case VOICE_ASSISTANT_PLAYBACK_ACTION_PAUSE:
            *turn_action = VOICE_PLAYBACK_TURN_ACTION_PAUSE;
            return true;
        case VOICE_ASSISTANT_PLAYBACK_ACTION_RESUME:
            *turn_action = VOICE_PLAYBACK_TURN_ACTION_RESUME;
            return true;
        case VOICE_ASSISTANT_PLAYBACK_ACTION_STOP:
            *turn_action = VOICE_PLAYBACK_TURN_ACTION_STOP;
            return true;
        case VOICE_ASSISTANT_PLAYBACK_ACTION_RESTART:
            *turn_action = VOICE_PLAYBACK_TURN_ACTION_RESTART;
            return true;
        default:
            return false;
    }
}

static esp_err_t playback_apply_action(
    voice_playback_turn_action_t action,
    uint32_t expected_generation)
{
    if (action == VOICE_PLAYBACK_TURN_ACTION_NONE) {
        return ESP_OK;
    }

    audio_manager_playback_status_t status = {0};
    const esp_err_t status_ret = audio_manager_get_playback_status(&status);
    if (status_ret != ESP_OK) {
        return status_ret;
    }
    if ((action != VOICE_PLAYBACK_TURN_ACTION_PLAY_RECORDED) &&
        ((expected_generation == 0U) ||
        (status.generation != expected_generation) ||
        !playback_source_is_local(status.source) || !status.resumable)) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (action) {
        case VOICE_PLAYBACK_TURN_ACTION_PAUSE:
            return audio_manager_pause_playback(
                AUDIO_MANAGER_PLAYBACK_PAUSE_USER);
        case VOICE_PLAYBACK_TURN_ACTION_RESUME:
        case VOICE_PLAYBACK_TURN_ACTION_AUTO_RESUME:
            return audio_manager_resume_playback(expected_generation);
        case VOICE_PLAYBACK_TURN_ACTION_STOP:
            return audio_manager_stop_playback();
        case VOICE_PLAYBACK_TURN_ACTION_RESTART:
            return audio_manager_restart_playback_at_generation(
                expected_generation);
        case VOICE_PLAYBACK_TURN_ACTION_PLAY_RECORDED: {
            if (playback_source_is_local(status.source) &&
                (status.state != AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)) {
                const esp_err_t stop_ret = audio_manager_stop_playback();
                if ((stop_ret != ESP_OK) && (stop_ret != ESP_ERR_INVALID_STATE)) return stop_ret;
            }
            for (uint32_t waited = 0U; waited <= VOICE_PLAYBACK_PAUSE_WAIT_MS;
                 waited += VOICE_PLAYBACK_POLL_MS) {
                audio_manager_status_t manager = {0};
                audio_manager_playback_arbiter_status_t arbiter = {0};
                if ((audio_manager_get_status(&manager) == ESP_OK) &&
                    (audio_manager_playback_arbiter_get_status(&arbiter) == ESP_OK) &&
                    (manager.state == AUDIO_MANAGER_STATE_IDLE) &&
                    !arbiter.current_valid && !arbiter.pending_valid) {
                    return audio_manager_play_recorded();
                }
                vTaskDelay(pdMS_TO_TICKS(VOICE_PLAYBACK_POLL_MS));
            }
            return ESP_ERR_TIMEOUT;
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t playback_finish_transaction(
    uint32_t ptt_generation,
    bool require_unstarted)
{
    voice_playback_turn_action_t action = VOICE_PLAYBACK_TURN_ACTION_NONE;
    uint32_t playback_generation = 0U;
    bool matched = false;

    portENTER_CRITICAL(&s_lock);
    matched = voice_playback_turn_policy_finish(
        &s_turn,
        ptt_generation,
        require_unstarted,
        &action,
        &playback_generation);
    portEXIT_CRITICAL(&s_lock);
    if (!matched) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = playback_apply_action(action, playback_generation);
    APP_LOGI(TAG, TURN_FINAL_GENERATION_U_ACTION_53FE1E8C,
             "turn final ptt_generation=%u action=%d playback_generation=%u result=%s",
             (unsigned)ptt_generation,
             (int)action,
             (unsigned)playback_generation,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t voice_assistant_playback_control_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        s_turn = (voice_playback_turn_policy_t) {0};
        s_initialized = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_playback_prepare_ptt(
    uint32_t ptt_generation,
    uint32_t session_generation)
{
    if ((ptt_generation == 0U) || (session_generation == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool already_current = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    already_current = s_turn.active &&
        (s_turn.ptt_generation == ptt_generation);
    if (!already_current && s_ptt_preparing) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!already_current) {
        s_ptt_preparing = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (already_current) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    uint32_t waited_ms = 0U;
    /* A catalog WAV is accepted by the arbiter before audio_manager has a
     * visible source. Drain that narrow ownership hand-off before PTT opens
     * capture; otherwise the WAV can begin after capture has been admitted. */
    for (;;) {
        bool submission_inflight = false;
        portENTER_CRITICAL(&s_lock);
        submission_inflight = (s_catalog_submissions_inflight != 0U);
        portEXIT_CRITICAL(&s_lock);
        if (submission_inflight) {
            if (waited_ms >= VOICE_PLAYBACK_PAUSE_WAIT_MS) {
                ret = ESP_ERR_TIMEOUT;
                goto preparation_done;
            }
            vTaskDelay(pdMS_TO_TICKS(VOICE_PLAYBACK_POLL_MS));
            waited_ms += VOICE_PLAYBACK_POLL_MS;
            continue;
        }

        uint32_t cancelled = 0U;
        const esp_err_t unstarted_ret =
            audio_manager_playback_arbiter_cancel_unstarted_wav_for_client(
                AUDIO_MANAGER_CLIENT_UI, &cancelled);
        if ((unstarted_ret != ESP_OK) && (unstarted_ret != ESP_ERR_NOT_FOUND)) {
            ret = unstarted_ret;
            goto preparation_done;
        }
        if (cancelled != 0U) {
            /* Re-read both owners after the atomic cancellation has published
             * its terminal record. */
            continue;
        }

        audio_manager_playback_status_t admission_playback = {0};
        audio_manager_playback_arbiter_status_t arbiter = {0};
        ret = audio_manager_get_playback_status(&admission_playback);
        if (ret == ESP_OK) {
            ret = audio_manager_playback_arbiter_get_status(&arbiter);
        }
        if (ret != ESP_OK) {
            goto preparation_done;
        }

        const bool unowned_catalog_dispatch = arbiter.current_valid &&
            (arbiter.current.client == AUDIO_MANAGER_CLIENT_UI) &&
            (arbiter.current.resource == AUDIO_MANAGER_RESOURCE_PLAYBACK) &&
            (admission_playback.source == AUDIO_MANAGER_PLAYBACK_SOURCE_NONE) &&
            (admission_playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE);
        if (!unowned_catalog_dispatch) {
            break;
        }

        const esp_err_t cancel_ret = audio_manager_playback_arbiter_cancel(
            arbiter.current.request_id);
        if ((cancel_ret != ESP_OK) && (cancel_ret != ESP_ERR_NOT_FOUND)) {
            ret = cancel_ret;
            goto preparation_done;
        }
        if (waited_ms >= VOICE_PLAYBACK_PAUSE_WAIT_MS) {
            ret = ESP_ERR_TIMEOUT;
            goto preparation_done;
        }
        vTaskDelay(pdMS_TO_TICKS(VOICE_PLAYBACK_POLL_MS));
        waited_ms += VOICE_PLAYBACK_POLL_MS;
    }

    audio_manager_playback_status_t playback = {0};
    ret = audio_manager_get_playback_status(&playback);
    if (ret != ESP_OK) {
        goto preparation_done;
    }

    bool has_local_source = playback_source_is_local(playback.source) &&
                            playback.resumable &&
                            (playback.generation != 0U);
    bool temporary_suspension = false;
    uint32_t playback_generation = has_local_source
        ? playback.generation
        : 0U;

    if ((playback.source == AUDIO_MANAGER_PLAYBACK_SOURCE_PCM16_STREAM) &&
        (playback.state != AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto preparation_done;
    }

    if (has_local_source &&
        ((playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING) ||
         (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING) ||
         (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING))) {
        ret = audio_manager_pause_playback(AUDIO_MANAGER_PLAYBACK_PAUSE_PTT);
        if (ret != ESP_OK) {
            goto preparation_done;
        }

        waited_ms = 0U;
        for (;;) {
            audio_manager_status_t manager = {0};
            ret = audio_manager_get_playback_status(&playback);
            const esp_err_t manager_ret = audio_manager_get_status(&manager);
            if ((ret != ESP_OK) || (manager_ret != ESP_OK)) {
                ret = (ret != ESP_OK) ? ret : manager_ret;
                break;
            }
            if ((playback.generation == playback_generation) &&
                (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED) &&
                (manager.state == AUDIO_MANAGER_STATE_IDLE) &&
                !manager.playback_i2s_active) {
                temporary_suspension = true;
                ret = ESP_OK;
                break;
            }
            if ((playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE) &&
                (playback.source == AUDIO_MANAGER_PLAYBACK_SOURCE_NONE)) {
                has_local_source = false;
                playback_generation = 0U;
                ret = ESP_OK;
                break;
            }
            if ((playback.generation != playback_generation) ||
                (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR)) {
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
            if (waited_ms >= VOICE_PLAYBACK_PAUSE_WAIT_MS) {
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(VOICE_PLAYBACK_POLL_MS));
            waited_ms += VOICE_PLAYBACK_POLL_MS;
        }
        if (ret != ESP_OK) {
            (void)audio_manager_resume_playback(playback_generation);
            goto preparation_done;
        }
    } else if (has_local_source &&
               (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED)) {
        temporary_suspension =
            (playback.pause_reason == AUDIO_MANAGER_PLAYBACK_PAUSE_PTT);
    } else if (has_local_source) {
        ret = ESP_ERR_INVALID_STATE;
        goto preparation_done;
    }

    portENTER_CRITICAL(&s_lock);
    voice_playback_turn_policy_begin(
        &s_turn,
        ptt_generation,
        session_generation,
        playback_generation,
        has_local_source,
        temporary_suspension);
    portEXIT_CRITICAL(&s_lock);

    APP_LOGI(TAG, PTT_PREPARED_GENERATION_U_LO_055455D4,
             "PTT prepared generation=%u local=%s temporary=%s playback_generation=%u",
             (unsigned)ptt_generation,
             has_local_source ? "yes" : "no",
             temporary_suspension ? "yes" : "no",
             (unsigned)playback_generation);
    ret = ESP_OK;

preparation_done:
    portENTER_CRITICAL(&s_lock);
    s_ptt_preparing = false;
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t voice_assistant_playback_supersede_ptt(
    uint32_t new_ptt_generation,
    uint32_t session_generation)
{
    bool superseded = false;
    portENTER_CRITICAL(&s_lock);
    superseded = s_initialized && voice_playback_turn_policy_supersede(
        &s_turn, new_ptt_generation, session_generation);
    portEXIT_CRITICAL(&s_lock);
    return superseded ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t voice_assistant_playback_mark_turn_started(uint32_t ptt_generation)
{
    bool marked = false;
    portENTER_CRITICAL(&s_lock);
    marked = s_initialized && voice_playback_turn_policy_mark_started(
        &s_turn, ptt_generation);
    portEXIT_CRITICAL(&s_lock);
    return marked ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t voice_assistant_playback_cancel_unstarted(uint32_t ptt_generation)
{
    return playback_finish_transaction(ptt_generation, true);
}

esp_err_t voice_assistant_playback_finish_turn(uint32_t ptt_generation)
{
    return playback_finish_transaction(ptt_generation, false);
}

esp_err_t voice_assistant_playback_control(
    voice_assistant_playback_action_t action,
    voice_assistant_playback_control_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = (voice_assistant_playback_control_result_t) {
        .outcome = VOICE_ASSISTANT_PLAYBACK_OUTCOME_CONTROL_FAILED,
    };
    if ((action < VOICE_ASSISTANT_PLAYBACK_ACTION_PAUSE) ||
        (action > VOICE_ASSISTANT_PLAYBACK_ACTION_RESTART)) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_manager_playback_status_t playback = {0};
    esp_err_t ret = audio_manager_get_playback_status(&playback);
    if (ret != ESP_OK) {
        result->outcome = playback_outcome_from_error(ret);
        return ESP_OK;
    }

    bool active_turn = false;
    bool has_local_source = false;
    voice_playback_turn_action_t deferred =
        VOICE_PLAYBACK_TURN_ACTION_NONE;
    if (!playback_action_to_turn_action(action, &deferred)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    active_turn = s_initialized && s_turn.active;
    has_local_source = active_turn && s_turn.has_local_source;
    if (has_local_source) {
        (void)voice_playback_turn_policy_set_override(&s_turn, deferred);
    }
    portEXIT_CRITICAL(&s_lock);

    if (active_turn) {
        if (!has_local_source) {
            result->outcome = VOICE_ASSISTANT_PLAYBACK_OUTCOME_NO_CURRENT_SOURCE;
            result->playback = playback;
            return ESP_OK;
        }
        result->outcome = VOICE_ASSISTANT_PLAYBACK_OUTCOME_SUCCESS;
        result->accepted = true;
        result->physically_applied = false;
        result->playback = playback;
        return ESP_OK;
    }

    if (!playback_source_is_local(playback.source) ||
        (playback.generation == 0U)) {
        result->outcome = (playback.source ==
                           AUDIO_MANAGER_PLAYBACK_SOURCE_PCM16_STREAM)
            ? VOICE_ASSISTANT_PLAYBACK_OUTCOME_NON_RESUMABLE_SOURCE
            : VOICE_ASSISTANT_PLAYBACK_OUTCOME_NO_CURRENT_SOURCE;
        result->playback = playback;
        return ESP_OK;
    }

    switch (action) {
        case VOICE_ASSISTANT_PLAYBACK_ACTION_PAUSE:
            ret = audio_manager_pause_playback(
                AUDIO_MANAGER_PLAYBACK_PAUSE_USER);
            break;
        case VOICE_ASSISTANT_PLAYBACK_ACTION_RESUME:
            ret = audio_manager_resume_playback(playback.generation);
            break;
        case VOICE_ASSISTANT_PLAYBACK_ACTION_STOP:
            ret = audio_manager_stop_playback();
            break;
        case VOICE_ASSISTANT_PLAYBACK_ACTION_RESTART:
            ret = audio_manager_restart_playback_at_generation(
                playback.generation);
            break;
        default:
            ret = ESP_ERR_INVALID_ARG;
            break;
    }

    result->outcome = (ret == ESP_OK)
        ? VOICE_ASSISTANT_PLAYBACK_OUTCOME_SUCCESS
        : playback_outcome_from_error(ret);
    result->accepted = (ret == ESP_OK);
    (void)audio_manager_get_playback_status(&result->playback);
    result->physically_applied =
        ((action == VOICE_ASSISTANT_PLAYBACK_ACTION_PAUSE) &&
         (result->playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED)) ||
        ((action == VOICE_ASSISTANT_PLAYBACK_ACTION_STOP) &&
         (result->playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)) ||
        (((action == VOICE_ASSISTANT_PLAYBACK_ACTION_RESUME) ||
          (action == VOICE_ASSISTANT_PLAYBACK_ACTION_RESTART)) &&
         (result->playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING));
    return ESP_OK;
}

esp_err_t voice_assistant_playback_get_status(
    audio_manager_playback_status_t *status)
{
    return audio_manager_get_playback_status(status);
}

esp_err_t voice_assistant_playback_start_catalog_wav(const char *resolved_path)
{
    if ((resolved_path == NULL) || (resolved_path[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    bool active_turn = false;
    bool has_local_source = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized || s_ptt_preparing) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    active_turn = s_initialized && s_turn.active;
    has_local_source = active_turn && s_turn.has_local_source;
    if (has_local_source) {
        /* STOP wins over the PTT auto-resume at terminal handling. The new
         * WAV is queued behind Xiaozhi, then promoted after this old owner is
         * cooperatively released. */
        (void)voice_playback_turn_policy_set_override(
            &s_turn, VOICE_PLAYBACK_TURN_ACTION_STOP);
    }
    uint32_t request_id = ++s_catalog_request_id;
    if (request_id == 0U) {
        request_id = ++s_catalog_request_id;
    }
    ++s_catalog_submissions_inflight;
    portEXIT_CRITICAL(&s_lock);

    if (!active_turn) {
        audio_manager_playback_status_t playback = {0};
        if (audio_manager_get_playback_status(&playback) == ESP_OK &&
            playback_source_is_local(playback.source) &&
            (playback.state != AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)) {
            const esp_err_t stop_ret = audio_manager_stop_playback();
            if ((stop_ret != ESP_OK) && (stop_ret != ESP_ERR_INVALID_STATE)) {
                portENTER_CRITICAL(&s_lock);
                --s_catalog_submissions_inflight;
                portEXIT_CRITICAL(&s_lock);
                return stop_ret;
            }
        }
    }
    const esp_err_t ret = audio_manager_play_catalog_wav(request_id, resolved_path);
    portENTER_CRITICAL(&s_lock);
    --s_catalog_submissions_inflight;
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t voice_assistant_playback_start_recorded(void)
{
    bool active_turn = false;
    portENTER_CRITICAL(&s_lock);
    active_turn = s_initialized && s_turn.active;
    if (active_turn) {
        (void)voice_playback_turn_policy_set_override(
            &s_turn, VOICE_PLAYBACK_TURN_ACTION_PLAY_RECORDED);
    }
    portEXIT_CRITICAL(&s_lock);
    if (active_turn) return ESP_OK;
    return audio_manager_play_recorded();
}
