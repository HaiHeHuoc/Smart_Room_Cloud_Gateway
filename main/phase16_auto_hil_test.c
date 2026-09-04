#include "phase16_auto_hil_test.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_gui.h"
#include "audio_manager.h"
#include "audio_manager_arbitration.h"
#include "audio_manager_capture_arbiter.h"
#include "audio_manager_named_playback.h"
#include "audio_manager_playback_arbiter.h"
#include "cloud_manager.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sd_card_manager.h"
#include "sensor_manager.h"
#include "voice_assistant.h"
#include "xiaozhi_foundation.h"

#if CONFIG_APP_PHASE16_AUTO_HIL_TEST

/*
 * This coordinator deliberately stays in main instead of audio_manager.  It
 * models application clients through the public arbiter interfaces and keeps
 * audio_manager as the only I2S/DMA/WAV-file owner.  The harness must never
 * become a hidden production scheduler or use raw GPIO/PTT input.
 */
#define PH16_AUTO_TASK_NAME                    "ph16_auto_hil"
#define PH16_AUTO_TASK_STACK_BYTES             6144U
#define PH16_AUTO_TASK_PRIORITY                5U
#define PH16_AUTO_POLL_MS                      50U
#define PH16_AUTO_SD_READY_TIMEOUT_MS          30000U
#define PH16_AUTO_VOICE_READY_TIMEOUT_MS       30000U
#define PH16_AUTO_ACTIVE_TIMEOUT_MS            12000U
#define PH16_AUTO_CAPTURE_TIMEOUT_MS           25000U
#define PH16_AUTO_CLEAN_TIMEOUT_MS             25000U
#define PH16_AUTO_GATEWAY_OBSERVE_MS           6000U
#define PH16_AUTO_RACE_SUBMIT_MAX_US            20000U

#define PH16_REQ_T05_XIAOZHI                   0x16050001U
#define PH16_REQ_T05_NOTIFICATION              0x16050002U
#define PH16_REQ_T06_XIAOZHI                   0x16060001U
#define PH16_REQ_T06_ALARM                     0x16060002U
#define PH16_REQ_T07_XIAOZHI                   0x16070001U
#define PH16_REQ_T07_EQUAL                     0x16070002U
#define PH16_REQ_T08_XIAOZHI                   0x16080001U
#define PH16_REQ_T08_NOTIFICATION              0x16080002U
#define PH16_REQ_T08_THIRD                     0x16080003U
#define PH16_REQ_T09_XIAOZHI                   0x16090001U
#define PH16_REQ_T09_EQUAL                     0x16090002U
#define PH16_REQ_T09_HIGHER                    0x16090003U
#define PH16_REQ_T10_CAPTURE                   0x16100001U
#define PH16_REQ_T10_PLAYBACK                  0x16100002U
#define PH16_REQ_T11_PRESTART                  0x16110001U
#define PH16_REQ_T11_XIAOZHI                   0x16110002U
#define PH16_REQ_T11_PENDING                   0x16110003U

static const char *const TAG = "PH16_TEST";
static TaskHandle_t s_task = NULL;

typedef enum {
    PH16_CASE_PASS = 0,
    PH16_CASE_FAIL,
    PH16_CASE_SKIP,
} ph16_case_outcome_t;

typedef struct {
    ph16_case_outcome_t outcome;
    const char *reason;
} ph16_case_result_t;

typedef struct {
    uint32_t passed;
    uint32_t failed;
    uint32_t skipped;
} ph16_summary_t;

typedef struct {
    size_t internal_free;
    size_t psram_free;
    size_t dma_free;
    uint32_t sensor_progress;
    sensor_manager_status_t sensor;
    cloud_manager_status_t cloud;
    sd_card_manager_status_t sd;
    app_gui_screen_id_t screen;
    voice_assistant_status_t voice;
    xiaozhi_foundation_session_status_t xiaozhi;
    audio_manager_status_t audio;
    audio_manager_playback_arbiter_status_t playback;
    audio_manager_capture_arbiter_status_t capture;
} ph16_gateway_snapshot_t;

static ph16_case_result_t ph16_pass(void)
{
    return (ph16_case_result_t) {
        .outcome = PH16_CASE_PASS,
        .reason = "runtime_evidence",
    };
}

static ph16_case_result_t ph16_fail(const char *reason)
{
    return (ph16_case_result_t) {
        .outcome = PH16_CASE_FAIL,
        .reason = (reason != NULL) ? reason : "unspecified",
    };
}

static ph16_case_result_t ph16_skip(const char *reason)
{
    return (ph16_case_result_t) {
        .outcome = PH16_CASE_SKIP,
        .reason = (reason != NULL) ? reason : "not_available",
    };
}

static void ph16_record_case(ph16_summary_t *summary,
                             uint32_t case_id,
                             ph16_case_result_t result)
{
    if (summary == NULL) {
        return;
    }

    switch (result.outcome) {
        case PH16_CASE_PASS:
            ++summary->passed;
            ESP_LOGI(TAG, "PH16_TEST T16_%02" PRIu32 " PASS", case_id);
            break;
        case PH16_CASE_SKIP:
            ++summary->skipped;
            ESP_LOGW(TAG,
                     "PH16_TEST T16_%02" PRIu32 " SKIP reason=%s",
                     case_id,
                     result.reason);
            break;
        case PH16_CASE_FAIL:
        default:
            ++summary->failed;
            ESP_LOGE(TAG,
                     "PH16_TEST T16_%02" PRIu32 " FAIL reason=%s",
                     case_id,
                     result.reason);
            break;
    }
}

static esp_err_t ph16_get_audio(audio_manager_status_t *status)
{
    return (status == NULL) ? ESP_ERR_INVALID_ARG : audio_manager_get_status(status);
}

static esp_err_t ph16_get_playback(audio_manager_playback_arbiter_status_t *status)
{
    return (status == NULL)
               ? ESP_ERR_INVALID_ARG
               : audio_manager_playback_arbiter_get_status(status);
}

static esp_err_t ph16_get_capture(audio_manager_capture_arbiter_status_t *status)
{
    return (status == NULL)
               ? ESP_ERR_INVALID_ARG
               : audio_manager_capture_arbiter_get_status(status);
}

static esp_err_t ph16_get_voice(voice_assistant_status_t *voice,
                                xiaozhi_foundation_session_status_t *xiaozhi)
{
    if ((voice == NULL) || (xiaozhi == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t voice_ret = voice_assistant_get_status(voice);
    const esp_err_t xiaozhi_ret = xiaozhi_foundation_session_get_status(xiaozhi);
    return (voice_ret != ESP_OK) ? voice_ret : xiaozhi_ret;
}

static esp_err_t ph16_wait_voice_ready(voice_assistant_status_t *voice,
                                       xiaozhi_foundation_session_status_t *xiaozhi)
{
    if ((voice == NULL) || (xiaozhi == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t waited_ms = 0U;
    while (waited_ms <= PH16_AUTO_VOICE_READY_TIMEOUT_MS) {
        voice_assistant_status_t current_voice = {0};
        xiaozhi_foundation_session_status_t current_xiaozhi = {0};
        const esp_err_t status_ret =
            ph16_get_voice(&current_voice, &current_xiaozhi);
        if (status_ret != ESP_OK) {
            return status_ret;
        }
        *voice = current_voice;
        *xiaozhi = current_xiaozhi;
        if ((current_voice.state == VOICE_ASSISTANT_STATE_READY) &&
            current_voice.session_active &&
            (current_xiaozhi.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
            current_xiaozhi.active) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(PH16_AUTO_POLL_MS));
        waited_ms += PH16_AUTO_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static void ph16_log_request(const char *label, const audio_manager_request_t *request)
{
    if ((label == NULL) || (request == NULL)) {
        return;
    }

    ESP_LOGI(TAG,
             "PH16_TEST %s request=%" PRIu32 " client=%s resource=%s priority=%u policy=%s interruptible=%s",
             label,
             request->request_id,
             audio_manager_client_to_string(request->client),
             audio_manager_resource_to_string(request->resource),
             (unsigned)request->priority,
             audio_manager_busy_policy_to_string(request->busy_policy),
             request->interruptible ? "yes" : "no");
}

static void ph16_log_playback_status(const char *label)
{
    audio_manager_playback_arbiter_status_t status = {0};
    if (ph16_get_playback(&status) != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG,
             "PH16_TEST %s PB state=%s current=%s/%" PRIu32 "/%s/%u/%s/%s pending=%s/%" PRIu32 "/%s/%u/%s/%s counters=a%" PRIu32 " r%" PRIu32 " q%" PRIu32 " p%" PRIu32 " c%" PRIu32 " f%" PRIu32 " err=%s",
             label,
             audio_manager_playback_arbiter_state_to_string(status.state),
             status.current_valid ? "yes" : "no",
             status.current_valid ? status.current.request_id : 0U,
             status.current_valid ? audio_manager_client_to_string(status.current.client) : "NONE",
             status.current_valid ? (unsigned)status.current.priority : 0U,
             status.current_valid ? audio_manager_busy_policy_to_string(status.current.busy_policy) : "NONE",
             (status.current_valid && status.current.interruptible) ? "yes" : "no",
             status.pending_valid ? "yes" : "no",
             status.pending_valid ? status.pending.request_id : 0U,
             status.pending_valid ? audio_manager_client_to_string(status.pending.client) : "NONE",
             status.pending_valid ? (unsigned)status.pending.priority : 0U,
             status.pending_valid ? audio_manager_busy_policy_to_string(status.pending.busy_policy) : "NONE",
             (status.pending_valid && status.pending.interruptible) ? "yes" : "no",
             status.accepted_count,
             status.rejected_count,
             status.queued_count,
             status.preemption_count,
             status.completed_count,
             status.failed_count,
             esp_err_to_name(status.last_error));
}

static void ph16_log_capture_status(const char *label)
{
    audio_manager_capture_arbiter_status_t status = {0};
    if (ph16_get_capture(&status) != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG,
             "PH16_TEST %s CAP state=%s current=%s/%" PRIu32 "/%s/%u/%s/%s pending=%s/%" PRIu32 "/%s/%u/%s/%s counters=a%" PRIu32 " r%" PRIu32 " q%" PRIu32 " p%" PRIu32 " c%" PRIu32 " f%" PRIu32 " err=%s",
             label,
             audio_manager_capture_arbiter_state_to_string(status.state),
             status.current_valid ? "yes" : "no",
             status.current_valid ? status.current.request_id : 0U,
             status.current_valid ? audio_manager_client_to_string(status.current.client) : "NONE",
             status.current_valid ? (unsigned)status.current.priority : 0U,
             status.current_valid ? audio_manager_busy_policy_to_string(status.current.busy_policy) : "NONE",
             (status.current_valid && status.current.interruptible) ? "yes" : "no",
             status.pending_valid ? "yes" : "no",
             status.pending_valid ? status.pending.request_id : 0U,
             status.pending_valid ? audio_manager_client_to_string(status.pending.client) : "NONE",
             status.pending_valid ? (unsigned)status.pending.priority : 0U,
             status.pending_valid ? audio_manager_busy_policy_to_string(status.pending.busy_policy) : "NONE",
             (status.pending_valid && status.pending.interruptible) ? "yes" : "no",
             status.accepted_count,
             status.rejected_count,
             status.queued_count,
             status.preemption_count,
             status.completed_count,
             status.failed_count,
             esp_err_to_name(status.last_error));
}

static void ph16_log_audio_status(const char *label)
{
    audio_manager_status_t status = {0};
    if (ph16_get_audio(&status) != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG,
             "PH16_TEST %s AUDIO state=%s capture_i2s=%s playback_i2s=%s wav=start%" PRIu32 "/done%" PRIu32 "/fail%" PRIu32 "/cancel%" PRIu32 " tx=qovf%" PRIu32 "/timeout%" PRIu32 "/partial%" PRIu32 " err=%s",
             label,
             audio_manager_state_to_string(status.state),
             status.capture_i2s_active ? "yes" : "no",
             status.playback_i2s_active ? "yes" : "no",
             status.wav_playback_started,
             status.wav_playback_completed,
             status.wav_playback_failed,
             status.wav_playback_cancelled,
             status.tx_queue_overflow_count,
             status.tx_timeout_count,
             status.tx_partial_write_count,
             esp_err_to_name(status.last_error));
}

static void ph16_log_voice_status(const char *label)
{
    voice_assistant_status_t voice = {0};
    xiaozhi_foundation_session_status_t xiaozhi = {0};
    if (ph16_get_voice(&voice, &xiaozhi) != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG,
             "PH16_TEST %s VOICE state=%s active=%s generation=%" PRIu32 " err=%s xiaozhi=%s active=%s generation=%" PRIu32 " err=%s",
             label,
             voice_assistant_state_to_string(voice.state),
             voice.session_active ? "yes" : "no",
             voice.session_generation,
             esp_err_to_name(voice.last_error),
             xiaozhi_foundation_session_state_to_string(xiaozhi.state),
             xiaozhi.active ? "yes" : "no",
             xiaozhi.client_generation,
             esp_err_to_name(xiaozhi.last_error));
}

static esp_err_t ph16_make_request(uint32_t request_id,
                                   audio_manager_client_t client,
                                   audio_manager_resource_t resource,
                                   uint8_t priority,
                                   audio_manager_busy_policy_t busy_policy,
                                   bool interruptible,
                                   audio_manager_request_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_manager_request_make_default(
        request_id,
        client,
        resource,
        request);
    if (ret != ESP_OK) {
        return ret;
    }

    request->priority = priority;
    request->busy_policy = busy_policy;
    request->interruptible = interruptible;
    return audio_manager_request_validate(request);
}

static esp_err_t ph16_wait_sd_ready(void)
{
    uint32_t waited_ms = 0U;
    while (waited_ms <= PH16_AUTO_SD_READY_TIMEOUT_MS) {
        if (sd_card_manager_is_mounted()) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(PH16_AUTO_POLL_MS));
        waited_ms += PH16_AUTO_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ph16_wait_playback_active(uint32_t request_id,
                                            uint32_t timeout_ms)
{
    uint32_t waited_ms = 0U;
    while (waited_ms <= timeout_ms) {
        audio_manager_playback_arbiter_status_t playback = {0};
        audio_manager_status_t audio = {0};
        const esp_err_t playback_ret = ph16_get_playback(&playback);
        const esp_err_t audio_ret = ph16_get_audio(&audio);
        if ((playback_ret != ESP_OK) || (audio_ret != ESP_OK)) {
            return (playback_ret != ESP_OK) ? playback_ret : audio_ret;
        }
        if (playback.state == AUDIO_MANAGER_PLAYBACK_ARBITER_ERROR) {
            return (playback.last_error == ESP_OK) ? ESP_FAIL : playback.last_error;
        }
        if (audio.state == AUDIO_MANAGER_STATE_ERROR) {
            return (audio.last_error == ESP_OK) ? ESP_FAIL : audio.last_error;
        }
        if (playback.current_valid &&
            (playback.current.request_id == request_id) &&
            (playback.state == AUDIO_MANAGER_PLAYBACK_ARBITER_ACTIVE) &&
            (audio.state == AUDIO_MANAGER_STATE_PLAYBACK) &&
            audio.playback_i2s_active) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(PH16_AUTO_POLL_MS));
        waited_ms += PH16_AUTO_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ph16_wait_capture_active(uint32_t request_id,
                                           uint32_t timeout_ms)
{
    uint32_t waited_ms = 0U;
    while (waited_ms <= timeout_ms) {
        audio_manager_capture_arbiter_status_t capture = {0};
        audio_manager_status_t audio = {0};
        const esp_err_t capture_ret = ph16_get_capture(&capture);
        const esp_err_t audio_ret = ph16_get_audio(&audio);
        if ((capture_ret != ESP_OK) || (audio_ret != ESP_OK)) {
            return (capture_ret != ESP_OK) ? capture_ret : audio_ret;
        }
        if (capture.state == AUDIO_MANAGER_CAPTURE_ARBITER_ERROR) {
            return (capture.last_error == ESP_OK) ? ESP_FAIL : capture.last_error;
        }
        if (audio.state == AUDIO_MANAGER_STATE_ERROR) {
            return (audio.last_error == ESP_OK) ? ESP_FAIL : audio.last_error;
        }
        if (capture.current_valid &&
            (capture.current.request_id == request_id) &&
            (capture.state == AUDIO_MANAGER_CAPTURE_ARBITER_ACTIVE) &&
            (audio.state == AUDIO_MANAGER_STATE_RECORDING) &&
            audio.capture_i2s_active) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(PH16_AUTO_POLL_MS));
        waited_ms += PH16_AUTO_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ph16_wait_clean(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0U;
    while (waited_ms <= timeout_ms) {
        audio_manager_status_t audio = {0};
        audio_manager_playback_arbiter_status_t playback = {0};
        audio_manager_capture_arbiter_status_t capture = {0};
        const esp_err_t audio_ret = ph16_get_audio(&audio);
        const esp_err_t playback_ret = ph16_get_playback(&playback);
        const esp_err_t capture_ret = ph16_get_capture(&capture);
        if ((audio_ret != ESP_OK) || (playback_ret != ESP_OK) ||
            (capture_ret != ESP_OK)) {
            return (audio_ret != ESP_OK)
                       ? audio_ret
                       : ((playback_ret != ESP_OK) ? playback_ret : capture_ret);
        }

        if ((audio.state == AUDIO_MANAGER_STATE_IDLE) &&
            !audio.capture_i2s_active && !audio.playback_i2s_active &&
            !playback.current_valid && !playback.pending_valid &&
            (playback.state == AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE) &&
            !capture.current_valid && !capture.pending_valid &&
            (capture.state == AUDIO_MANAGER_CAPTURE_ARBITER_IDLE)) {
            return ESP_OK;
        }

        if ((audio.state == AUDIO_MANAGER_STATE_ERROR) ||
            (playback.state == AUDIO_MANAGER_PLAYBACK_ARBITER_ERROR) ||
            (capture.state == AUDIO_MANAGER_CAPTURE_ARBITER_ERROR)) {
            return ESP_FAIL;
        }

        vTaskDelay(pdMS_TO_TICKS(PH16_AUTO_POLL_MS));
        waited_ms += PH16_AUTO_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ph16_cancel_playback(uint32_t request_id)
{
    const esp_err_t ret = audio_manager_playback_arbiter_cancel(request_id);
    if ((ret != ESP_OK) && (ret != ESP_ERR_NOT_FOUND)) {
        return ret;
    }
    return ESP_OK;
}

static esp_err_t ph16_cancel_capture(uint32_t request_id)
{
    const esp_err_t ret = audio_manager_capture_arbiter_cancel(request_id);
    if ((ret != ESP_OK) && (ret != ESP_ERR_NOT_FOUND)) {
        return ret;
    }
    return ESP_OK;
}

static esp_err_t ph16_start_synthetic_xiaozhi(uint32_t request_id)
{
    audio_manager_request_t request = {0};
    esp_err_t ret = ph16_make_request(
        request_id,
        AUDIO_MANAGER_CLIENT_XIAOZHI,
        AUDIO_MANAGER_RESOURCE_PLAYBACK,
        AUDIO_MANAGER_PRIORITY_XIAOZHI,
        AUDIO_MANAGER_BUSY_QUEUE,
        true,
        &request);
    if (ret != ESP_OK) {
        return ret;
    }

    ph16_log_request("SYNTHETIC_XIAOZHI", &request);
    ret = audio_manager_playback_arbiter_submit_wav(
        &request,
        CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);
    if (ret != ESP_OK) {
        return ret;
    }
    return ph16_wait_playback_active(request_id, PH16_AUTO_ACTIVE_TIMEOUT_MS);
}

static ph16_case_result_t ph16_case_boot(void)
{
    ESP_LOGI(TAG, "PH16_TEST T16_02 BEGIN");
    audio_manager_status_t audio = {0};
    audio_manager_playback_arbiter_status_t playback = {0};
    audio_manager_capture_arbiter_status_t capture = {0};
    app_gui_screen_id_t screen = APP_GUI_SCREEN_NONE;
    voice_assistant_status_t voice = {0};
    xiaozhi_foundation_session_status_t xiaozhi = {0};

    if ((ph16_get_audio(&audio) != ESP_OK) ||
        (ph16_get_playback(&playback) != ESP_OK) ||
        (ph16_get_capture(&capture) != ESP_OK) ||
        (app_gui_get_screen_id(&screen) != ESP_OK)) {
        return ph16_fail("startup_status_unavailable");
    }
    if (ph16_wait_voice_ready(&voice, &xiaozhi) != ESP_OK) {
        ph16_log_voice_status("T16_02_NOT_READY");
        return ph16_fail("voice_stack_not_ready");
    }

    ph16_log_audio_status("T16_02");
    ph16_log_playback_status("T16_02");
    ph16_log_capture_status("T16_02");
    ph16_log_voice_status("T16_02");

    if ((audio.state != AUDIO_MANAGER_STATE_IDLE) ||
        (playback.state != AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE) ||
        (capture.state != AUDIO_MANAGER_CAPTURE_ARBITER_IDLE) ||
        (screen == APP_GUI_SCREEN_BOOT) ||
        (screen == APP_GUI_SCREEN_NONE) ||
        (voice.state != VOICE_ASSISTANT_STATE_READY) ||
        !voice.session_active ||
        (xiaozhi.state != XIAOZHI_FOUNDATION_SESSION_READY) ||
        !xiaozhi.active) {
        return ph16_fail("startup_not_quiescent");
    }

    return ph16_pass();
}

static ph16_case_result_t ph16_case_notification_queue(bool wav_ready)
{
    ESP_LOGI(TAG, "PH16_TEST T16_05 BEGIN");
    if (!wav_ready) {
        return ph16_skip("sd_vfs_not_ready");
    }

    audio_manager_playback_arbiter_status_t before = {0};
    if ((ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) ||
        (ph16_get_playback(&before) != ESP_OK)) {
        return ph16_fail("precondition_not_clean");
    }

    esp_err_t ret = ph16_start_synthetic_xiaozhi(PH16_REQ_T05_XIAOZHI);
    if (ret != ESP_OK) {
        ph16_log_playback_status("T16_05_START_FAIL");
        return ph16_fail("xiaozhi_playback_not_active");
    }

    ret = audio_manager_play_notification_wav(
        PH16_REQ_T05_NOTIFICATION,
        CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);
    audio_manager_playback_arbiter_status_t after = {0};
    const esp_err_t status_ret = ph16_get_playback(&after);
    ph16_log_playback_status("T16_05");
    ph16_log_audio_status("T16_05");

    const bool accepted_without_preempt =
        (ret == ESP_OK) && (status_ret == ESP_OK) &&
        after.current_valid &&
        (after.current.request_id == PH16_REQ_T05_XIAOZHI) &&
        (after.current.client == AUDIO_MANAGER_CLIENT_XIAOZHI) &&
        after.pending_valid &&
        (after.pending.request_id == PH16_REQ_T05_NOTIFICATION) &&
        (after.pending.client == AUDIO_MANAGER_CLIENT_NOTIFICATION) &&
        (after.preemption_count == before.preemption_count) &&
        (after.queued_count >= (before.queued_count + 1U));

    const esp_err_t pending_cancel = ph16_cancel_playback(PH16_REQ_T05_NOTIFICATION);
    const esp_err_t current_cancel = ph16_cancel_playback(PH16_REQ_T05_XIAOZHI);
    const esp_err_t clean_ret = ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
    if ((pending_cancel != ESP_OK) || (current_cancel != ESP_OK) ||
        (clean_ret != ESP_OK)) {
        return ph16_fail("notification_cleanup_failed");
    }

    return accepted_without_preempt ? ph16_pass()
                                    : ph16_fail("notification_preempted_or_not_queued");
}

static ph16_case_result_t ph16_case_alarm_preempt(bool wav_ready)
{
    ESP_LOGI(TAG, "PH16_TEST T16_06 BEGIN");
    if (!wav_ready) {
        return ph16_skip("sd_vfs_not_ready");
    }

    audio_manager_playback_arbiter_status_t before = {0};
    if ((ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) ||
        (ph16_get_playback(&before) != ESP_OK)) {
        return ph16_fail("precondition_not_clean");
    }

    if (ph16_start_synthetic_xiaozhi(PH16_REQ_T06_XIAOZHI) != ESP_OK) {
        return ph16_fail("xiaozhi_playback_not_active");
    }

    const esp_err_t submit_ret = audio_manager_play_critical_alarm_wav(
        PH16_REQ_T06_ALARM,
        CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);
    const esp_err_t active_ret = (submit_ret == ESP_OK)
                                     ? ph16_wait_playback_active(
                                           PH16_REQ_T06_ALARM,
                                           PH16_AUTO_ACTIVE_TIMEOUT_MS)
                                     : submit_ret;
    audio_manager_playback_arbiter_status_t after = {0};
    const esp_err_t status_ret = ph16_get_playback(&after);
    ph16_log_playback_status("T16_06");
    ph16_log_audio_status("T16_06");

    const bool preempted = (active_ret == ESP_OK) && (status_ret == ESP_OK) &&
                           after.current_valid &&
                           (after.current.request_id == PH16_REQ_T06_ALARM) &&
                           (after.current.client == AUDIO_MANAGER_CLIENT_ALARM) &&
                           (after.current.priority == AUDIO_MANAGER_PRIORITY_CRITICAL_ALARM) &&
                           !after.current.interruptible &&
                           (after.preemption_count >= (before.preemption_count + 1U));

    const esp_err_t alarm_cancel = ph16_cancel_playback(PH16_REQ_T06_ALARM);
    const esp_err_t xiaozhi_cancel = ph16_cancel_playback(PH16_REQ_T06_XIAOZHI);
    const esp_err_t clean_ret = ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
    if ((alarm_cancel != ESP_OK) || (xiaozhi_cancel != ESP_OK) ||
        (clean_ret != ESP_OK)) {
        return ph16_fail("alarm_cleanup_failed");
    }

    return preempted ? ph16_pass() : ph16_fail("alarm_not_promoted_after_preempt");
}

static ph16_case_result_t ph16_case_equal_priority(bool wav_ready)
{
    ESP_LOGI(TAG, "PH16_TEST T16_07 BEGIN");
    if (!wav_ready) {
        return ph16_skip("sd_vfs_not_ready");
    }

    audio_manager_playback_arbiter_status_t before = {0};
    if ((ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) ||
        (ph16_get_playback(&before) != ESP_OK) ||
        (ph16_start_synthetic_xiaozhi(PH16_REQ_T07_XIAOZHI) != ESP_OK)) {
        return ph16_fail("xiaozhi_playback_not_active");
    }

    audio_manager_request_t equal = {0};
    const esp_err_t make_ret = ph16_make_request(
        PH16_REQ_T07_EQUAL,
        AUDIO_MANAGER_CLIENT_TEST,
        AUDIO_MANAGER_RESOURCE_PLAYBACK,
        AUDIO_MANAGER_PRIORITY_XIAOZHI,
        AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY,
        true,
        &equal);
    const esp_err_t submit_ret = (make_ret == ESP_OK)
                                     ? audio_manager_playback_arbiter_submit_wav(
                                           &equal,
                                           CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH)
                                     : make_ret;
    audio_manager_playback_arbiter_status_t after = {0};
    const esp_err_t status_ret = ph16_get_playback(&after);
    ph16_log_request("T16_07_EQUAL", &equal);
    ph16_log_playback_status("T16_07");

    const bool rejected_without_preempt =
        (submit_ret == ESP_ERR_INVALID_STATE) && (status_ret == ESP_OK) &&
        after.current_valid &&
        (after.current.request_id == PH16_REQ_T07_XIAOZHI) &&
        !after.pending_valid &&
        (after.preemption_count == before.preemption_count) &&
        (after.rejected_count >= (before.rejected_count + 1U));

    const esp_err_t cancel_ret = ph16_cancel_playback(PH16_REQ_T07_XIAOZHI);
    const esp_err_t clean_ret = ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
    if ((cancel_ret != ESP_OK) || (clean_ret != ESP_OK)) {
        return ph16_fail("equal_priority_cleanup_failed");
    }

    return rejected_without_preempt ? ph16_pass()
                                    : ph16_fail("equal_priority_preempted_or_queued");
}

static ph16_case_result_t ph16_case_queue_pressure(bool wav_ready)
{
    ESP_LOGI(TAG, "PH16_TEST T16_08 BEGIN");
    if (!wav_ready) {
        return ph16_skip("sd_vfs_not_ready");
    }

    audio_manager_playback_arbiter_status_t before = {0};
    if ((ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) ||
        (ph16_get_playback(&before) != ESP_OK) ||
        (ph16_start_synthetic_xiaozhi(PH16_REQ_T08_XIAOZHI) != ESP_OK)) {
        return ph16_fail("xiaozhi_playback_not_active");
    }

    const esp_err_t notification_ret = audio_manager_play_notification_wav(
        PH16_REQ_T08_NOTIFICATION,
        CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);

    audio_manager_request_t third = {0};
    const esp_err_t make_ret = ph16_make_request(
        PH16_REQ_T08_THIRD,
        AUDIO_MANAGER_CLIENT_UI,
        AUDIO_MANAGER_RESOURCE_PLAYBACK,
        AUDIO_MANAGER_PRIORITY_NOTIFICATION,
        AUDIO_MANAGER_BUSY_QUEUE,
        true,
        &third);
    const esp_err_t third_ret = (make_ret == ESP_OK)
                                    ? audio_manager_playback_arbiter_submit_wav(
                                          &third,
                                          CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH)
                                    : make_ret;
    audio_manager_playback_arbiter_status_t after = {0};
    const esp_err_t status_ret = ph16_get_playback(&after);
    ph16_log_request("T16_08_THIRD", &third);
    ph16_log_playback_status("T16_08");

    const bool bounded_reject =
        (notification_ret == ESP_OK) &&
        (third_ret == ESP_ERR_INVALID_STATE) && (status_ret == ESP_OK) &&
        after.current_valid &&
        (after.current.request_id == PH16_REQ_T08_XIAOZHI) &&
        after.pending_valid &&
        (after.pending.request_id == PH16_REQ_T08_NOTIFICATION) &&
        (after.queued_count >= (before.queued_count + 1U)) &&
        (after.rejected_count >= (before.rejected_count + 1U));

    const esp_err_t pending_cancel = ph16_cancel_playback(PH16_REQ_T08_NOTIFICATION);
    const esp_err_t current_cancel = ph16_cancel_playback(PH16_REQ_T08_XIAOZHI);
    const esp_err_t clean_ret = ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
    if ((pending_cancel != ESP_OK) || (current_cancel != ESP_OK) ||
        (clean_ret != ESP_OK)) {
        return ph16_fail("queue_pressure_cleanup_failed");
    }

    return bounded_reject ? ph16_pass() : ph16_fail("third_request_not_rejected");
}

static ph16_case_result_t ph16_case_capture_contention(void)
{
    ESP_LOGI(TAG, "PH16_TEST T16_09 BEGIN");
    if (ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) {
        return ph16_fail("precondition_not_clean");
    }

    audio_manager_capture_arbiter_status_t before = {0};
    if (ph16_get_capture(&before) != ESP_OK) {
        return ph16_fail("capture_status_unavailable");
    }

    audio_manager_request_t xiaozhi = {0};
    if (ph16_make_request(
            PH16_REQ_T09_XIAOZHI,
            AUDIO_MANAGER_CLIENT_XIAOZHI,
            AUDIO_MANAGER_RESOURCE_CAPTURE,
            AUDIO_MANAGER_PRIORITY_XIAOZHI,
            AUDIO_MANAGER_BUSY_REJECT,
            true,
            &xiaozhi) != ESP_OK) {
        return ph16_fail("xiaozhi_request_invalid");
    }
    ph16_log_request("T16_09_XIAOZHI", &xiaozhi);
    if ((audio_manager_capture_arbiter_submit(&xiaozhi) != ESP_OK) ||
        (ph16_wait_capture_active(PH16_REQ_T09_XIAOZHI,
                                  PH16_AUTO_CAPTURE_TIMEOUT_MS) != ESP_OK)) {
        ph16_log_capture_status("T16_09_XIAOZHI_FAIL");
        return ph16_fail("xiaozhi_capture_not_active");
    }

    audio_manager_request_t equal = {0};
    if (ph16_make_request(
            PH16_REQ_T09_EQUAL,
            AUDIO_MANAGER_CLIENT_TEST,
            AUDIO_MANAGER_RESOURCE_CAPTURE,
            AUDIO_MANAGER_PRIORITY_XIAOZHI,
            AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY,
            true,
            &equal) != ESP_OK) {
        (void)ph16_cancel_capture(PH16_REQ_T09_XIAOZHI);
        (void)ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
        return ph16_fail("equal_capture_request_invalid");
    }
    const esp_err_t equal_ret = audio_manager_capture_arbiter_submit(&equal);

    audio_manager_request_t higher = {0};
    if (ph16_make_request(
            PH16_REQ_T09_HIGHER,
            AUDIO_MANAGER_CLIENT_SYSTEM,
            AUDIO_MANAGER_RESOURCE_CAPTURE,
            AUDIO_MANAGER_PRIORITY_SYSTEM,
            AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY,
            false,
            &higher) != ESP_OK) {
        (void)ph16_cancel_capture(PH16_REQ_T09_XIAOZHI);
        (void)ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
        return ph16_fail("higher_capture_request_invalid");
    }
    ph16_log_request("T16_09_EQUAL", &equal);
    ph16_log_request("T16_09_HIGHER", &higher);
    const esp_err_t higher_ret = audio_manager_capture_arbiter_submit(&higher);
    const esp_err_t higher_active = (higher_ret == ESP_OK)
                                        ? ph16_wait_capture_active(
                                              PH16_REQ_T09_HIGHER,
                                              PH16_AUTO_CAPTURE_TIMEOUT_MS)
                                        : higher_ret;
    audio_manager_capture_arbiter_status_t after = {0};
    const esp_err_t status_ret = ph16_get_capture(&after);
    ph16_log_capture_status("T16_09");
    ph16_log_audio_status("T16_09");

    const bool contention_policy_ok =
        (equal_ret == ESP_ERR_INVALID_STATE) && (higher_active == ESP_OK) &&
        (status_ret == ESP_OK) && after.current_valid &&
        (after.current.request_id == PH16_REQ_T09_HIGHER) &&
        (after.current.priority == AUDIO_MANAGER_PRIORITY_SYSTEM) &&
        (after.preemption_count >= (before.preemption_count + 1U));

    const esp_err_t high_cancel = ph16_cancel_capture(PH16_REQ_T09_HIGHER);
    const esp_err_t xiaozhi_cancel = ph16_cancel_capture(PH16_REQ_T09_XIAOZHI);
    const esp_err_t clean_ret = ph16_wait_clean(PH16_AUTO_CAPTURE_TIMEOUT_MS);
    if ((high_cancel != ESP_OK) || (xiaozhi_cancel != ESP_OK) ||
        (clean_ret != ESP_OK)) {
        return ph16_fail("capture_contention_cleanup_failed");
    }

    return contention_policy_ok ? ph16_pass()
                                : ph16_fail("capture_contention_policy_failed");
}

static ph16_case_result_t ph16_case_cross_resource_race(bool wav_ready)
{
    ESP_LOGI(TAG, "PH16_TEST T16_10 BEGIN");
    if (!wav_ready) {
        return ph16_skip("sd_vfs_not_ready");
    }
    if (ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) {
        return ph16_fail("precondition_not_clean");
    }

    audio_manager_request_t capture = {0};
    audio_manager_request_t playback = {0};
    if ((ph16_make_request(
             PH16_REQ_T10_CAPTURE,
             AUDIO_MANAGER_CLIENT_TEST,
             AUDIO_MANAGER_RESOURCE_CAPTURE,
             AUDIO_MANAGER_PRIORITY_BACKGROUND,
             AUDIO_MANAGER_BUSY_REJECT,
             true,
             &capture) != ESP_OK) ||
        (ph16_make_request(
             PH16_REQ_T10_PLAYBACK,
             AUDIO_MANAGER_CLIENT_TEST,
             AUDIO_MANAGER_RESOURCE_PLAYBACK,
             AUDIO_MANAGER_PRIORITY_BACKGROUND,
             AUDIO_MANAGER_BUSY_REJECT,
             true,
             &playback) != ESP_OK)) {
        return ph16_fail("race_request_invalid");
    }

    const int64_t capture_submit_at_us = esp_timer_get_time();
    const esp_err_t capture_ret = audio_manager_capture_arbiter_submit(&capture);
    const int64_t playback_submit_at_us = esp_timer_get_time();
    const esp_err_t playback_ret = audio_manager_playback_arbiter_submit_wav(
        &playback,
        CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);
    const uint32_t submit_delta_us =
        (playback_submit_at_us >= capture_submit_at_us)
            ? (uint32_t)(playback_submit_at_us - capture_submit_at_us)
            : UINT32_MAX;
    ph16_log_request("T16_10_CAPTURE", &capture);
    ph16_log_request("T16_10_PLAYBACK", &playback);
    ESP_LOGI(TAG,
             "PH16_TEST T16_10 submit_delta_us=%" PRIu32 " capture_ret=%s playback_ret=%s",
             submit_delta_us,
             esp_err_to_name(capture_ret),
             esp_err_to_name(playback_ret));

    if ((capture_ret != ESP_OK) || (playback_ret != ESP_OK)) {
        (void)ph16_cancel_capture(PH16_REQ_T10_CAPTURE);
        (void)ph16_cancel_playback(PH16_REQ_T10_PLAYBACK);
        (void)ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
        return ph16_fail("race_submit_failed");
    }

    uint32_t waited_ms = 0U;
    bool serialized_wait_observed = false;
    bool duplicate_owner_observed = false;
    bool capture_won = false;
    while (waited_ms <= PH16_AUTO_ACTIVE_TIMEOUT_MS) {
        audio_manager_status_t audio = {0};
        audio_manager_capture_arbiter_status_t cap = {0};
        audio_manager_playback_arbiter_status_t pb = {0};
        if ((ph16_get_audio(&audio) != ESP_OK) ||
            (ph16_get_capture(&cap) != ESP_OK) ||
            (ph16_get_playback(&pb) != ESP_OK)) {
            break;
        }

        const bool capture_active =
            (cap.state == AUDIO_MANAGER_CAPTURE_ARBITER_ACTIVE) &&
            (audio.state == AUDIO_MANAGER_STATE_RECORDING) &&
            audio.capture_i2s_active;
        const bool playback_active =
            (pb.state == AUDIO_MANAGER_PLAYBACK_ARBITER_ACTIVE) &&
            (audio.state == AUDIO_MANAGER_STATE_PLAYBACK) &&
            audio.playback_i2s_active;
        if ((capture_active && playback_active) ||
            (audio.capture_i2s_active && audio.playback_i2s_active)) {
            duplicate_owner_observed = true;
            break;
        }

        /* A losing arbiter can either reach the manager and receive busy, or
         * observe the already-active opposite resource before issuing a
         * command. Both retain the logical request until manager IDLE. */
        const bool capture_waiting_for_manager =
            cap.current_valid &&
            (cap.current.request_id == PH16_REQ_T10_CAPTURE) &&
            (cap.state == AUDIO_MANAGER_CAPTURE_ARBITER_IDLE) &&
            ((cap.last_error == ESP_OK) ||
             (cap.last_error == ESP_ERR_INVALID_STATE));
        const bool playback_waiting_for_manager =
            pb.current_valid &&
            (pb.current.request_id == PH16_REQ_T10_PLAYBACK) &&
            (pb.state == AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE) &&
            ((pb.last_error == ESP_OK) ||
             (pb.last_error == ESP_ERR_INVALID_STATE));

        if (capture_active && playback_waiting_for_manager) {
            capture_won = true;
            serialized_wait_observed = true;
            ESP_LOGI(TAG,
                     "PH16_TEST T16_10 winner=CAPTURE loser=PLAYBACK wait=%s",
                     (pb.last_error == ESP_ERR_INVALID_STATE)
                         ? "RETRY_AFTER_INVALID_STATE"
                         : "WAIT_STATUS_BUSY");
            break;
        }
        if (playback_active && capture_waiting_for_manager) {
            capture_won = false;
            serialized_wait_observed = true;
            ESP_LOGI(TAG,
                     "PH16_TEST T16_10 winner=PLAYBACK loser=CAPTURE wait=%s",
                     (cap.last_error == ESP_ERR_INVALID_STATE)
                         ? "RETRY_AFTER_INVALID_STATE"
                         : "WAIT_STATUS_BUSY");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PH16_AUTO_POLL_MS));
        waited_ms += PH16_AUTO_POLL_MS;
    }

    ph16_log_capture_status("T16_10");
    ph16_log_playback_status("T16_10");
    ph16_log_audio_status("T16_10");
    if (!serialized_wait_observed) {
        const esp_err_t capture_cancel =
            ph16_cancel_capture(PH16_REQ_T10_CAPTURE);
        const esp_err_t playback_cancel =
            ph16_cancel_playback(PH16_REQ_T10_PLAYBACK);
        const esp_err_t clean_ret =
            ph16_wait_clean(PH16_AUTO_CAPTURE_TIMEOUT_MS);
        if ((capture_cancel != ESP_OK) || (playback_cancel != ESP_OK) ||
            (clean_ret != ESP_OK)) {
            return ph16_fail("race_cleanup_failed");
        }
        return duplicate_owner_observed
                   ? ph16_fail("duplicate_i2s_owner_observed")
                   : ph16_fail("race_no_serialized_wait_observed");
    }

    const esp_err_t winner_cancel = capture_won
                                        ? ph16_cancel_capture(PH16_REQ_T10_CAPTURE)
                                        : ph16_cancel_playback(PH16_REQ_T10_PLAYBACK);
    const esp_err_t loser_active =
        (winner_cancel == ESP_OK)
            ? (capture_won
                   ? ph16_wait_playback_active(
                         PH16_REQ_T10_PLAYBACK,
                         PH16_AUTO_ACTIVE_TIMEOUT_MS)
                   : ph16_wait_capture_active(
                         PH16_REQ_T10_CAPTURE,
                         PH16_AUTO_CAPTURE_TIMEOUT_MS))
            : winner_cancel;
    const esp_err_t loser_cancel =
        (loser_active == ESP_OK)
            ? (capture_won
                   ? ph16_cancel_playback(PH16_REQ_T10_PLAYBACK)
                   : ph16_cancel_capture(PH16_REQ_T10_CAPTURE))
            : ESP_FAIL;
    const esp_err_t clean_ret = ph16_wait_clean(PH16_AUTO_CAPTURE_TIMEOUT_MS);
    if ((winner_cancel != ESP_OK) || (loser_active != ESP_OK) ||
        (loser_cancel != ESP_OK) || (clean_ret != ESP_OK)) {
        return ph16_fail("race_cleanup_failed");
    }

    if (submit_delta_us > PH16_AUTO_RACE_SUBMIT_MAX_US) {
        return ph16_fail("race_submission_not_near_simultaneous");
    }
    return ph16_pass();
}

static ph16_case_result_t ph16_case_cancellation(bool wav_ready)
{
    ESP_LOGI(TAG, "PH16_TEST T16_11 BEGIN");
    if (!wav_ready) {
        return ph16_skip("sd_vfs_not_ready");
    }
    if (ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) {
        return ph16_fail("precondition_not_clean");
    }

    audio_manager_request_t prestart = {0};
    if (ph16_make_request(
             PH16_REQ_T11_PRESTART,
             AUDIO_MANAGER_CLIENT_TEST,
             AUDIO_MANAGER_RESOURCE_PLAYBACK,
             AUDIO_MANAGER_PRIORITY_BACKGROUND,
             AUDIO_MANAGER_BUSY_QUEUE,
             true,
             &prestart) != ESP_OK) {
        return ph16_fail("prestart_setup_failed");
    }

    /* Keep the test request in the arbiter pending slot so cancellation is
     * demonstrably before the manager receives a WAV command, even on SMP. */
    if (ph16_start_synthetic_xiaozhi(PH16_REQ_T11_XIAOZHI) != ESP_OK) {
        return ph16_fail("active_cancel_start_failed");
    }
    audio_manager_status_t audio_before_prestart = {0};
    if (ph16_get_audio(&audio_before_prestart) != ESP_OK) {
        (void)ph16_cancel_playback(PH16_REQ_T11_XIAOZHI);
        (void)ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
        return ph16_fail("prestart_audio_status_unavailable");
    }

    const esp_err_t prestart_submit = audio_manager_playback_arbiter_submit_wav(
        &prestart,
        CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);
    audio_manager_playback_arbiter_status_t prestart_pending = {0};
    const esp_err_t prestart_pending_status = ph16_get_playback(&prestart_pending);
    const esp_err_t prestart_cancel = (prestart_submit == ESP_OK)
                                           ? ph16_cancel_playback(PH16_REQ_T11_PRESTART)
                                           : prestart_submit;
    audio_manager_status_t audio_after_prestart = {0};
    const esp_err_t prestart_status = ph16_get_audio(&audio_after_prestart);

    const bool prestart_cancelled =
        (prestart_submit == ESP_OK) && (prestart_cancel == ESP_OK) &&
        (prestart_pending_status == ESP_OK) && prestart_pending.current_valid &&
        (prestart_pending.current.request_id == PH16_REQ_T11_XIAOZHI) &&
        prestart_pending.pending_valid &&
        (prestart_pending.pending.request_id == PH16_REQ_T11_PRESTART) &&
        (prestart_status == ESP_OK) &&
        (audio_after_prestart.wav_playback_started ==
         audio_before_prestart.wav_playback_started);

    if (!prestart_cancelled) {
        ph16_log_audio_status("T16_11_PRESTART");
        const esp_err_t prestart_cleanup =
            ph16_cancel_playback(PH16_REQ_T11_PRESTART);
        const esp_err_t active_cleanup =
            ph16_cancel_playback(PH16_REQ_T11_XIAOZHI);
        const esp_err_t clean_ret = ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
        if ((prestart_cleanup != ESP_OK) || (active_cleanup != ESP_OK) ||
            (clean_ret != ESP_OK)) {
            return ph16_fail("prestart_cancel_cleanup_failed");
        }
        return ph16_fail("prestart_cancel_touched_hardware");
    }

    const esp_err_t pending_submit = audio_manager_play_notification_wav(
        PH16_REQ_T11_PENDING,
        CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);
    const esp_err_t pending_cancel = (pending_submit == ESP_OK)
                                         ? ph16_cancel_playback(PH16_REQ_T11_PENDING)
                                         : pending_submit;
    audio_manager_playback_arbiter_status_t after_pending_cancel = {0};
    const esp_err_t pending_status = ph16_get_playback(&after_pending_cancel);

    const bool pending_removed =
        (pending_submit == ESP_OK) && (pending_cancel == ESP_OK) &&
        (pending_status == ESP_OK) &&
        after_pending_cancel.current_valid &&
        (after_pending_cancel.current.request_id == PH16_REQ_T11_XIAOZHI) &&
        !after_pending_cancel.pending_valid;

    const esp_err_t active_cancel = ph16_cancel_playback(PH16_REQ_T11_XIAOZHI);
    const esp_err_t active_clean = ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS);
    ph16_log_playback_status("T16_11");
    ph16_log_audio_status("T16_11");

    if ((active_cancel != ESP_OK) || (active_clean != ESP_OK)) {
        return ph16_fail("active_cancel_cleanup_failed");
    }
    return pending_removed ? ph16_pass() : ph16_fail("pending_cancel_not_isolated");
}

static esp_err_t ph16_capture_gateway_snapshot(ph16_gateway_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = (ph16_gateway_snapshot_t) {0};
    snapshot->internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);

    if ((sensor_manager_get_status(&snapshot->sensor) != ESP_OK) ||
        (cloud_manager_get_status(&snapshot->cloud) != ESP_OK) ||
        (sd_card_manager_get_status(&snapshot->sd) != ESP_OK) ||
        (app_gui_get_screen_id(&snapshot->screen) != ESP_OK) ||
        (ph16_get_voice(&snapshot->voice, &snapshot->xiaozhi) != ESP_OK) ||
        (ph16_get_audio(&snapshot->audio) != ESP_OK) ||
        (ph16_get_playback(&snapshot->playback) != ESP_OK) ||
        (ph16_get_capture(&snapshot->capture) != ESP_OK)) {
        return ESP_FAIL;
    }

    snapshot->sensor_progress = snapshot->sensor.successful_read_count +
                                snapshot->sensor.failed_read_count;
    return ESP_OK;
}

static ph16_case_result_t ph16_case_full_gateway(
    bool wav_ready,
    const ph16_gateway_snapshot_t *before)
{
    ESP_LOGI(TAG, "PH16_TEST T16_12 BEGIN");
    if (!wav_ready) {
        return ph16_skip("sd_vfs_not_ready");
    }
    if (before == NULL) {
        return ph16_fail("baseline_unavailable");
    }

    vTaskDelay(pdMS_TO_TICKS(PH16_AUTO_GATEWAY_OBSERVE_MS));
    ph16_gateway_snapshot_t after = {0};
    if ((ph16_wait_clean(PH16_AUTO_CLEAN_TIMEOUT_MS) != ESP_OK) ||
        (ph16_capture_gateway_snapshot(&after) != ESP_OK)) {
        return ph16_fail("post_test_snapshot_unavailable");
    }

    const int64_t internal_delta = (int64_t)after.internal_free -
                                   (int64_t)before->internal_free;
    const int64_t psram_delta = (int64_t)after.psram_free -
                                (int64_t)before->psram_free;
    const int64_t dma_delta = (int64_t)after.dma_free -
                              (int64_t)before->dma_free;
    ESP_LOGI(TAG,
             "PH16_TEST T16_12 gateway sensor_progress=%" PRIu32 "->%" PRIu32 " cloud_state=%d->%d sd_state=%d->%d gui=%d->%d voice=%s xiaozhi=%s heap_delta_internal=%" PRId64 " psram=%" PRId64 " dma=%" PRId64 " pb_fail=%" PRIu32 " cap_fail=%" PRIu32,
             before->sensor_progress,
             after.sensor_progress,
             (int)before->cloud.state,
             (int)after.cloud.state,
             (int)before->sd.state,
             (int)after.sd.state,
             (int)before->screen,
             (int)after.screen,
             voice_assistant_state_to_string(after.voice.state),
             xiaozhi_foundation_session_state_to_string(after.xiaozhi.state),
             internal_delta,
             psram_delta,
             dma_delta,
             after.playback.failed_count,
             after.capture.failed_count);

    const bool services_progressed =
        (after.sensor_progress > before->sensor_progress) &&
        (after.cloud.state != CLOUD_MANAGER_STATE_UNINITIALIZED) &&
        (after.sd.state == SD_CARD_MANAGER_STATE_READY) &&
        (after.screen != APP_GUI_SCREEN_NONE) &&
        (after.screen != APP_GUI_SCREEN_BOOT) &&
        (after.voice.state == VOICE_ASSISTANT_STATE_READY) &&
        after.voice.session_active &&
        (after.xiaozhi.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
        after.xiaozhi.active &&
        (after.audio.state == AUDIO_MANAGER_STATE_IDLE) &&
        !after.playback.current_valid && !after.playback.pending_valid &&
        !after.capture.current_valid && !after.capture.pending_valid;

    return services_progressed ? ph16_pass()
                               : ph16_fail("gateway_service_or_audio_state_stalled");
}

static void ph16_auto_hil_task(void *argument)
{
    (void)argument;
    ph16_summary_t summary = {0};
    ESP_LOGI(TAG,
             "PH16_TEST AUTO_BEGIN wav_path=%s gpio_ptt=unused i2s_owner=audio_manager_only",
             CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);

    /* T16-01 is established by the invoking ESP-IDF build. */
    ESP_LOGI(TAG, "PH16_TEST T16_01 PASS evidence=build_linked_auto_harness");
    ++summary.passed;

    const ph16_case_result_t boot_result = ph16_case_boot();
    ph16_record_case(&summary, 2U, boot_result);
    ph16_record_case(&summary, 3U,
                     ph16_skip("requires_real_xiaozhi_ptt_and_microphone_turn"));
    ph16_record_case(&summary, 4U,
                     ph16_skip("requires_real_xiaozhi_network_tts_and_speaker_evidence"));

    /* Synthetic arbitration must not manufacture PASS evidence when the
     * production voice/Xiaozhi startup boundary did not become READY. */
    if (boot_result.outcome != PH16_CASE_PASS) {
        ESP_LOGE(TAG,
                 "PH16_TEST arbitration cases skipped because T16_02 boot precondition failed");
        for (uint32_t case_id = 5U; case_id <= 12U; ++case_id) {
            ph16_record_case(&summary, case_id,
                             ph16_skip("boot_precondition_failed"));
        }
        ph16_log_audio_status("SUMMARY");
        ph16_log_playback_status("SUMMARY");
        ph16_log_capture_status("SUMMARY");
        ESP_LOGI(TAG,
                 "PH16_TEST SUMMARY pass=%" PRIu32 " fail=%" PRIu32 " skip=%" PRIu32,
                 summary.passed,
                 summary.failed,
                 summary.skipped);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const bool wav_ready = (ph16_wait_sd_ready() == ESP_OK);
    if (!wav_ready) {
        ESP_LOGW(TAG,
                 "PH16_TEST WAV prerequisite unavailable path=%s; playback cases will SKIP",
                 CONFIG_APP_PHASE16_AUTO_HIL_WAV_PATH);
    }

    ph16_gateway_snapshot_t gateway_before = {0};
    const bool gateway_baseline_ready =
        (ph16_capture_gateway_snapshot(&gateway_before) == ESP_OK);
    if (!gateway_baseline_ready) {
        ESP_LOGW(TAG, "PH16_TEST T16_12 baseline snapshot unavailable");
    }

    ph16_record_case(&summary, 5U, ph16_case_notification_queue(wav_ready));
    ph16_record_case(&summary, 6U, ph16_case_alarm_preempt(wav_ready));
    ph16_record_case(&summary, 7U, ph16_case_equal_priority(wav_ready));
    ph16_record_case(&summary, 8U, ph16_case_queue_pressure(wav_ready));
    ph16_record_case(&summary, 9U, ph16_case_capture_contention());
    ph16_record_case(&summary, 10U, ph16_case_cross_resource_race(wav_ready));
    ph16_record_case(&summary, 11U, ph16_case_cancellation(wav_ready));
    ph16_record_case(&summary, 12U,
                     ph16_case_full_gateway(wav_ready,
                                            gateway_baseline_ready
                                                ? &gateway_before
                                                : NULL));

    ph16_log_audio_status("SUMMARY");
    ph16_log_playback_status("SUMMARY");
    ph16_log_capture_status("SUMMARY");
    ESP_LOGI(TAG,
             "PH16_TEST SUMMARY pass=%" PRIu32 " fail=%" PRIu32 " skip=%" PRIu32,
             summary.passed,
             summary.failed,
             summary.skipped);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_phase16_auto_hil_test_start(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(ph16_auto_hil_task,
                    PH16_AUTO_TASK_NAME,
                    PH16_AUTO_TASK_STACK_BYTES,
                    NULL,
                    PH16_AUTO_TASK_PRIORITY,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else

esp_err_t app_phase16_auto_hil_test_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_APP_PHASE16_AUTO_HIL_TEST */
