#include "xiaozhi_mcp_audio_playback.h"
#include "xiaozhi_mcp_audio_playback_policy.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_foundation.h"

static const char *const TAG = "XZ_AUDIO_MCP";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_audio_control_provider_t s_control_provider = NULL;
static void *s_control_context = NULL;
static xiaozhi_foundation_audio_state_provider_t s_state_provider = NULL;
static void *s_state_context = NULL;
static xiaozhi_foundation_audio_track_list_provider_t s_track_list_provider = NULL;
static void *s_track_list_context = NULL;
static xiaozhi_foundation_audio_track_play_provider_t s_track_play_provider = NULL;
static void *s_track_play_context = NULL;
static xiaozhi_foundation_audio_recorded_play_provider_t s_recorded_play_provider = NULL;
static void *s_recorded_play_context = NULL;
static bool s_attached = false;

static const char *audio_state_name(xiaozhi_foundation_audio_state_t state)
{
    switch (state) {
        case XIAOZHI_FOUNDATION_AUDIO_STATE_IDLE: return "idle";
        case XIAOZHI_FOUNDATION_AUDIO_STATE_STARTING: return "starting";
        case XIAOZHI_FOUNDATION_AUDIO_STATE_PLAYING: return "playing";
        case XIAOZHI_FOUNDATION_AUDIO_STATE_PAUSING: return "pausing";
        case XIAOZHI_FOUNDATION_AUDIO_STATE_PAUSED: return "paused";
        case XIAOZHI_FOUNDATION_AUDIO_STATE_RESUMING: return "resuming";
        case XIAOZHI_FOUNDATION_AUDIO_STATE_STOPPING: return "stopping";
        case XIAOZHI_FOUNDATION_AUDIO_STATE_ERROR: return "error";
        default: return NULL;
    }
}

static const char *audio_source_name(xiaozhi_foundation_audio_source_t source)
{
    switch (source) {
        case XIAOZHI_FOUNDATION_AUDIO_SOURCE_NONE: return "none";
        case XIAOZHI_FOUNDATION_AUDIO_SOURCE_RECORDED: return "recorded";
        case XIAOZHI_FOUNDATION_AUDIO_SOURCE_WAV: return "wav";
        case XIAOZHI_FOUNDATION_AUDIO_SOURCE_LIVE_PCM: return "live_pcm";
        default: return NULL;
    }
}

static const char *audio_pause_reason_name(
    xiaozhi_foundation_audio_pause_reason_t reason)
{
    switch (reason) {
        case XIAOZHI_FOUNDATION_AUDIO_PAUSE_NONE: return "none";
        case XIAOZHI_FOUNDATION_AUDIO_PAUSE_USER: return "user";
        case XIAOZHI_FOUNDATION_AUDIO_PAUSE_PTT_TEMPORARY:
            return "ptt_temporary";
        default: return NULL;
    }
}

static const char *audio_outcome_error(
    xiaozhi_foundation_audio_outcome_t outcome)
{
    switch (outcome) {
        case XIAOZHI_FOUNDATION_AUDIO_OUTCOME_NO_CURRENT_SOURCE:
            return "no_current_source";
        case XIAOZHI_FOUNDATION_AUDIO_OUTCOME_INVALID_STATE:
            return "invalid_state";
        case XIAOZHI_FOUNDATION_AUDIO_OUTCOME_NON_RESUMABLE_SOURCE:
            return "non_resumable_source";
        case XIAOZHI_FOUNDATION_AUDIO_OUTCOME_STALE_GENERATION:
            return "stale_generation";
        case XIAOZHI_FOUNDATION_AUDIO_OUTCOME_CONTROL_FAILED:
            return "control_failed";
        case XIAOZHI_FOUNDATION_AUDIO_OUTCOME_SUCCESS:
        default:
            return "internal_error";
    }
}

static esp_err_t audio_set_error(
    esp_mcp_tool_result_t *result,
    const char *error_code)
{
    char json[112] = {0};
    char text[144] = {0};
    const int json_written = snprintf(
        json, sizeof(json),
        "{\"success\":false,\"accepted\":false,\"physically_applied\":false,\"error_code\":\"%s\"}",
        error_code);
    const int text_written = snprintf(
        text, sizeof(text),
        "SMART_ROOM_AUDIO_CONTROL_ERROR: %s.", error_code);
    if ((json_written < 0) || ((size_t)json_written >= sizeof(json)) ||
        (text_written < 0) || ((size_t)text_written >= sizeof(text))) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = esp_mcp_tool_result_set_is_error(result, true);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_set_structured_json(result, json);
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}

static esp_err_t audio_format_snapshot(
    const xiaozhi_foundation_audio_playback_snapshot_t *snapshot,
    bool include_control,
    bool accepted,
    bool applied,
    char *json,
    size_t json_size,
    char *text,
    size_t text_size)
{
    const char *const state = audio_state_name(snapshot->state);
    const char *const source = audio_source_name(snapshot->source_type);
    const char *const reason = audio_pause_reason_name(snapshot->pause_reason);
    if ((state == NULL) || (source == NULL) || (reason == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    const int json_written = include_control
        ? snprintf(json, json_size,
                   "{\"success\":true,\"accepted\":%s,\"physically_applied\":%s,\"error_code\":null,\"state\":\"%s\",\"source_type\":\"%s\",\"pause_reason\":\"%s\",\"resumable\":%s,\"generation\":%u,\"position_frames\":%llu,\"total_frames\":%llu,\"position_granularity_frames\":%u}",
                   accepted ? "true" : "false",
                   applied ? "true" : "false",
                   state, source, reason,
                   snapshot->resumable ? "true" : "false",
                   (unsigned)snapshot->generation,
                   (unsigned long long)snapshot->position_frames,
                   (unsigned long long)snapshot->total_frames,
                   (unsigned)snapshot->position_granularity_frames)
        : snprintf(json, json_size,
                   "{\"state_available\":%s,\"state\":\"%s\",\"source_type\":\"%s\",\"pause_reason\":\"%s\",\"resumable\":%s,\"generation\":%u,\"position_frames\":%llu,\"total_frames\":%llu,\"position_granularity_frames\":%u}",
                   snapshot->available ? "true" : "false",
                   state, source, reason,
                   snapshot->resumable ? "true" : "false",
                   (unsigned)snapshot->generation,
                   (unsigned long long)snapshot->position_frames,
                   (unsigned long long)snapshot->total_frames,
                   (unsigned)snapshot->position_granularity_frames);
    const int text_written = snprintf(
        text, text_size,
        "SMART_ROOM_AUDIO_PLAYBACK: state=%s; source_type=%s; pause_reason=%s; resumable=%s; generation=%u; position_frames=%llu; total_frames=%llu; position_granularity_frames=%u%s",
        state, source, reason,
        snapshot->resumable ? "true" : "false",
        (unsigned)snapshot->generation,
        (unsigned long long)snapshot->position_frames,
        (unsigned long long)snapshot->total_frames,
        (unsigned)snapshot->position_granularity_frames,
        include_control
            ? (applied ? "; command physically applied." :
               "; command accepted; physical transition is deferred or pending.")
            : ".");
    return ((json_written < 0) || ((size_t)json_written >= json_size) ||
            (text_written < 0) || ((size_t)text_written >= text_size))
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}

static esp_err_t audio_control_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *const action_text = (properties == NULL) ? NULL :
        esp_mcp_property_list_get_property_string(properties, "action");
    xiaozhi_foundation_audio_action_t action = {0};
    if (!xiaozhi_mcp_audio_playback_parse_action(action_text, &action)) {
        return audio_set_error(
            result,
            (action_text == NULL) ? "missing_or_wrong_type_action" :
                                    "invalid_action");
    }

    xiaozhi_foundation_audio_control_provider_t provider = NULL;
    void *context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_control_provider;
    context = s_control_context;
    portEXIT_CRITICAL(&s_lock);
    if (provider == NULL) {
        return audio_set_error(result, "provider_unavailable");
    }

    xiaozhi_foundation_audio_control_result_t control = {0};
    const esp_err_t provider_ret = provider(action, &control, context);
    if ((provider_ret != ESP_OK) ||
        (control.outcome != XIAOZHI_FOUNDATION_AUDIO_OUTCOME_SUCCESS)) {
        return audio_set_error(
            result,
            (provider_ret == ESP_OK)
                ? audio_outcome_error(control.outcome)
                : "provider_failed");
    }

    char json[400] = {0};
    char text[320] = {0};
    esp_err_t ret = audio_format_snapshot(
        &control.playback, true, control.accepted,
        control.physically_applied,
        json, sizeof(json), text, sizeof(text));
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_set_structured_json(result, json);
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}

static esp_err_t audio_state_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xiaozhi_foundation_audio_state_provider_t provider = NULL;
    void *context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_state_provider;
    context = s_state_context;
    portEXIT_CRITICAL(&s_lock);
    if (provider == NULL) {
        return audio_set_error(result, "provider_unavailable");
    }

    xiaozhi_foundation_audio_playback_snapshot_t snapshot = {0};
    if (provider(&snapshot, context) != ESP_OK) {
        return audio_set_error(result, "snapshot_failed");
    }
    char json[336] = {0};
    char text[288] = {0};
    esp_err_t ret = audio_format_snapshot(
        &snapshot, false, false, false,
        json, sizeof(json), text, sizeof(text));
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_set_structured_json(result, json);
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}

static const char *audio_track_error(
    xiaozhi_foundation_audio_track_outcome_t outcome)
{
    switch (outcome) {
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_INVALID_REQUEST: return "invalid_request";
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_NOT_FOUND: return "track_not_found";
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_STORAGE_UNAVAILABLE: return "storage_unavailable";
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_CATALOG_UNAVAILABLE: return "catalog_unavailable";
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_PLAYBACK_REJECTED: return "playback_rejected";
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_RECORDED_AUDIO_NOT_AVAILABLE: return "recorded_audio_not_available";
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_INTERNAL_ERROR:
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS:
        default: return "internal_error";
    }
}

static bool audio_track_token_is_safe(const char *value, size_t max_bytes)
{
    if (value == NULL) return false;
    const size_t length = strnlen(value, max_bytes);
    if ((length == 0U) || (length >= max_bytes)) return false;
    for (size_t i = 0U; i < length; ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || (c == '_') || (c == '-'))) return false;
    }
    return true;
}

static esp_err_t audio_list_tracks_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    xiaozhi_foundation_audio_track_list_provider_t provider = NULL;
    void *context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_track_list_provider;
    context = s_track_list_context;
    portEXIT_CRITICAL(&s_lock);
    xiaozhi_foundation_audio_track_list_t tracks = {0};
    if ((provider == NULL) || (provider(&tracks, context) != ESP_OK) ||
        !tracks.available ||
        (tracks.track_count > XIAOZHI_FOUNDATION_AUDIO_TRACK_MAX_COUNT)) {
        return audio_set_error(result, "catalog_unavailable");
    }
    for (uint8_t i = 0U; i < tracks.track_count; ++i) {
        if (!audio_track_token_is_safe(tracks.tracks[i].id,
                                       XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES) ||
            !audio_track_token_is_safe(tracks.tracks[i].name,
                                       XIAOZHI_FOUNDATION_AUDIO_TRACK_NAME_MAX_BYTES)) {
            return audio_set_error(result, "catalog_unavailable");
        }
    }
    char json[1536] = {0};
    size_t used = 0U;
    int written = snprintf(json, sizeof(json),
                           "{\"catalog_available\":true,\"truncated\":%s,\"tracks\":[",
                           tracks.truncated ? "true" : "false");
    if ((written < 0) || ((size_t)written >= sizeof(json))) return ESP_ERR_INVALID_SIZE;
    used = (size_t)written;
    for (uint8_t i = 0U; i < tracks.track_count; ++i) {
        written = snprintf(json + used, sizeof(json) - used,
                           "%s{\"id\":\"%s\",\"name\":\"%s\"}",
                           (i == 0U) ? "" : ",", tracks.tracks[i].id,
                           tracks.tracks[i].name);
        if ((written < 0) || ((size_t)written >= (sizeof(json) - used))) return ESP_ERR_INVALID_SIZE;
        used += (size_t)written;
    }
    written = snprintf(json + used, sizeof(json) - used, "]}");
    if ((written < 0) || ((size_t)written >= (sizeof(json) - used))) return ESP_ERR_INVALID_SIZE;
    char text[192] = {0};
    written = snprintf(text, sizeof(text),
                       "SMART_ROOM_AUDIO_TRACKS: %u bounded logical tracks are available. Use only an exact returned id with audio.play_track.",
                       (unsigned)tracks.track_count);
    if ((written < 0) || ((size_t)written >= sizeof(text))) return ESP_ERR_INVALID_SIZE;
    esp_err_t ret = esp_mcp_tool_result_set_structured_json(result, json);
    if (ret == ESP_OK) ret = esp_mcp_tool_result_add_text(result, text);
    return ret;
}

static esp_err_t audio_play_track_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    const char *id = (properties == NULL) ? NULL :
        esp_mcp_property_list_get_property_string(properties, "track_id");
    if (!audio_track_token_is_safe(id, XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES))
        return audio_set_error(result, "invalid_request");
    xiaozhi_foundation_audio_track_play_provider_t provider = NULL;
    void *context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_track_play_provider;
    context = s_track_play_context;
    portEXIT_CRITICAL(&s_lock);
    xiaozhi_foundation_audio_track_play_result_t play = {0};
    const esp_err_t ret = (provider == NULL) ? ESP_ERR_INVALID_STATE :
        provider(id, &play, context);
    if ((ret != ESP_OK) || (play.outcome != XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS)) {
        return audio_set_error(result, (ret == ESP_OK) ? audio_track_error(play.outcome) : "internal_error");
    }
    return esp_mcp_tool_result_set_structured_json(
        result, "{\"success\":true,\"accepted\":true,\"scheduled\":true,\"error_code\":null}");
}

static esp_err_t audio_play_recorded_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    xiaozhi_foundation_audio_recorded_play_provider_t provider = NULL;
    void *context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_recorded_play_provider;
    context = s_recorded_play_context;
    portEXIT_CRITICAL(&s_lock);
    xiaozhi_foundation_audio_track_play_result_t play = {0};
    const esp_err_t ret = (provider == NULL) ? ESP_ERR_INVALID_STATE :
        provider(&play, context);
    if ((ret != ESP_OK) || (play.outcome != XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS)) {
        return audio_set_error(result, (ret == ESP_OK) ? audio_track_error(play.outcome) : "internal_error");
    }
    return esp_mcp_tool_result_set_structured_json(
        result, "{\"success\":true,\"accepted\":true,\"scheduled\":true,\"error_code\":null}");
}

esp_err_t xiaozhi_foundation_register_audio_control_provider(
    xiaozhi_foundation_audio_control_provider_t provider,
    void *user_context)
{
    if (provider == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_attached) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_control_provider = provider;
    s_control_context = user_context;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t xiaozhi_foundation_register_audio_state_provider(
    xiaozhi_foundation_audio_state_provider_t provider,
    void *user_context)
{
    if (provider == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_attached) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state_provider = provider;
    s_state_context = user_context;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t xiaozhi_foundation_register_audio_track_list_provider(
    xiaozhi_foundation_audio_track_list_provider_t provider, void *user_context)
{
    if (provider == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_attached) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    s_track_list_provider = provider;
    s_track_list_context = user_context;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t xiaozhi_foundation_register_audio_track_play_provider(
    xiaozhi_foundation_audio_track_play_provider_t provider, void *user_context)
{
    if (provider == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_attached) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    s_track_play_provider = provider;
    s_track_play_context = user_context;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t xiaozhi_foundation_register_audio_recorded_play_provider(
    xiaozhi_foundation_audio_recorded_play_provider_t provider, void *user_context)
{
    if (provider == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_attached) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    s_recorded_play_provider = provider;
    s_recorded_play_context = user_context;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void xiaozhi_mcp_audio_playback_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_audio_playback_attach(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    const bool ready = (s_control_provider != NULL) &&
                       (s_state_provider != NULL) &&
                       (s_track_list_provider != NULL) &&
                       (s_track_play_provider != NULL) &&
                       (s_recorded_play_provider != NULL) && !s_attached;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mcp_tool_t *control = esp_mcp_tool_create_ex(
        "audio.control_playback",
        "Smart Room: Dieu khien phat am thanh",
        "Control only the existing Smart Room playback source. action must be exactly pause, resume, stop, or restart. This tool cannot select files or tracks. During a PTT voice turn, resume/restart are accepted as final intent and apply only after the spoken response completes, preventing overlap with Xiaozhi TTS.",
        audio_control_callback);
    if (control == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_mcp_property_t *action = esp_mcp_property_create(
        "action", ESP_MCP_PROPERTY_TYPE_STRING);
    if (action == NULL) {
        (void)esp_mcp_tool_destroy(control);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = esp_mcp_tool_add_property(control, action);
    const bool action_added = (ret == ESP_OK);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_output_schema_json(
            control,
            "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"},\"accepted\":{\"type\":\"boolean\"},\"physically_applied\":{\"type\":\"boolean\"},\"error_code\":{\"type\":[\"string\",\"null\"]},\"state\":{\"type\":\"string\"},\"source_type\":{\"type\":\"string\"},\"pause_reason\":{\"type\":\"string\"},\"resumable\":{\"type\":\"boolean\"},\"generation\":{\"type\":\"integer\"},\"position_frames\":{\"type\":\"integer\"},\"total_frames\":{\"type\":\"integer\"},\"position_granularity_frames\":{\"type\":\"integer\"}},\"required\":[\"success\",\"accepted\",\"physically_applied\"]}");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_annotations_json(
            control,
            "{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":false,\"openWorldHint\":false}");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_task_support(control, "optional");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_add_tool(mcp, control);
    }
    if (ret != ESP_OK) {
        if (!action_added) {
            (void)esp_mcp_property_destroy(action);
        }
        (void)esp_mcp_tool_destroy(control);
        return ret;
    }

    esp_mcp_tool_t *list_tracks = esp_mcp_tool_create_ex(
        "audio.list_tracks", "Smart Room: Danh sach bai hat",
        "List the bounded logical audio tracks directly available in the Smart Room SD audio catalog. This is read-only. Use the returned exact id for audio.play_track; never invent a path or id.",
        audio_list_tracks_callback);
    if (list_tracks == NULL) return ESP_ERR_NO_MEM;
    ret = esp_mcp_tool_set_output_schema_json(list_tracks,
        "{\"type\":\"object\",\"properties\":{\"catalog_available\":{\"type\":\"boolean\"},\"truncated\":{\"type\":\"boolean\"},\"tracks\":{\"type\":\"array\"}},\"required\":[\"catalog_available\",\"truncated\",\"tracks\"]}");
    if (ret == ESP_OK) ret = esp_mcp_tool_set_annotations_json(list_tracks,
        "{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}");
    if (ret == ESP_OK) ret = esp_mcp_tool_set_task_support(list_tracks, "optional");
    if (ret == ESP_OK) ret = esp_mcp_add_tool(mcp, list_tracks);
    if (ret != ESP_OK) { (void)esp_mcp_tool_destroy(list_tracks); return ret; }

    esp_mcp_tool_t *play_track = esp_mcp_tool_create_ex(
        "audio.play_track", "Smart Room: Phat bai hat",
        "Play one track selected from audio.list_tracks. track_id must be exactly one returned logical id. The device resolves it internally and never accepts a filesystem path.",
        audio_play_track_callback);
    if (play_track == NULL) return ESP_ERR_NO_MEM;
    esp_mcp_property_t *track_id = esp_mcp_property_create("track_id", ESP_MCP_PROPERTY_TYPE_STRING);
    if (track_id == NULL) { (void)esp_mcp_tool_destroy(play_track); return ESP_ERR_NO_MEM; }
    ret = esp_mcp_tool_add_property(play_track, track_id);
    const bool id_added = (ret == ESP_OK);
    if (ret == ESP_OK) ret = esp_mcp_tool_set_output_schema_json(play_track,
        "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"},\"accepted\":{\"type\":\"boolean\"},\"scheduled\":{\"type\":\"boolean\"},\"error_code\":{\"type\":[\"string\",\"null\"]}},\"required\":[\"success\",\"accepted\",\"scheduled\"]}");
    if (ret == ESP_OK) ret = esp_mcp_tool_set_annotations_json(play_track,
        "{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":false,\"openWorldHint\":false}");
    if (ret == ESP_OK) ret = esp_mcp_tool_set_task_support(play_track, "optional");
    if (ret == ESP_OK) ret = esp_mcp_add_tool(mcp, play_track);
    if (ret != ESP_OK) { if (!id_added) (void)esp_mcp_property_destroy(track_id); (void)esp_mcp_tool_destroy(play_track); return ret; }

    esp_mcp_tool_t *play_recorded = esp_mcp_tool_create_ex(
        "audio.play_recorded", "Smart Room: Phat ban ghi am gan nhat",
        "Play the retained processed recording if one exists. This does not create a recording or expose audio buffers. The request is ordered after the current Xiaozhi response.",
        audio_play_recorded_callback);
    if (play_recorded == NULL) return ESP_ERR_NO_MEM;
    ret = esp_mcp_tool_set_output_schema_json(play_recorded,
        "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"},\"accepted\":{\"type\":\"boolean\"},\"scheduled\":{\"type\":\"boolean\"},\"error_code\":{\"type\":[\"string\",\"null\"]}},\"required\":[\"success\",\"accepted\",\"scheduled\"]}");
    if (ret == ESP_OK) ret = esp_mcp_tool_set_annotations_json(play_recorded,
        "{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":false,\"openWorldHint\":false}");
    if (ret == ESP_OK) ret = esp_mcp_tool_set_task_support(play_recorded, "optional");
    if (ret == ESP_OK) ret = esp_mcp_add_tool(mcp, play_recorded);
    if (ret != ESP_OK) { (void)esp_mcp_tool_destroy(play_recorded); return ret; }

    esp_mcp_tool_t *state = esp_mcp_tool_create_ex(
        "audio.get_playback_state",
        "Smart Room: Trang thai phat am thanh",
        "Read the current copied Smart Room playback state. Always use this before answering questions about playback state, source type, resumability, or position. It has no side effects and exposes no path, handle, pointer, or PCM data.",
        audio_state_callback);
    if (state == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ret = esp_mcp_tool_set_output_schema_json(
        state,
        "{\"type\":\"object\",\"properties\":{\"state_available\":{\"type\":\"boolean\"},\"state\":{\"type\":\"string\",\"enum\":[\"idle\",\"starting\",\"playing\",\"pausing\",\"paused\",\"resuming\",\"stopping\",\"error\"]},\"source_type\":{\"type\":\"string\",\"enum\":[\"none\",\"recorded\",\"wav\",\"live_pcm\"]},\"pause_reason\":{\"type\":\"string\",\"enum\":[\"none\",\"user\",\"ptt_temporary\"]},\"resumable\":{\"type\":\"boolean\"},\"generation\":{\"type\":\"integer\"},\"position_frames\":{\"type\":\"integer\"},\"total_frames\":{\"type\":\"integer\"},\"position_granularity_frames\":{\"type\":\"integer\"}},\"required\":[\"state_available\",\"state\",\"source_type\",\"pause_reason\",\"resumable\",\"generation\",\"position_frames\",\"total_frames\",\"position_granularity_frames\"]}");
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_annotations_json(
            state,
            "{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_task_support(state, "optional");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_add_tool(mcp, state);
    }
    if (ret != ESP_OK) {
        (void)esp_mcp_tool_destroy(state);
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    s_attached = true;
    portEXIT_CRITICAL(&s_lock);
    APP_LOGI(TAG, AUDIO_PLAYBACK_TOOLS_REGISTER_77CBFDF3,
             "audio.control_playback and audio.get_playback_state registered");
    return ESP_OK;
}
