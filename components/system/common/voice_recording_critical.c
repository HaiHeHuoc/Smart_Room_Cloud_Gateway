#include "voice_recording_critical.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define VOICE_RECORDING_CRITICAL_MAX_LISTENERS 4U

typedef struct {
    voice_recording_critical_listener_t listener;
    void *context;
} voice_recording_critical_listener_slot_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static voice_recording_critical_status_t s_status = {0};
static voice_recording_critical_listener_slot_t
    s_listeners[VOICE_RECORDING_CRITICAL_MAX_LISTENERS] = {0};

static void voice_recording_critical_notify_listeners(
    bool active,
    uint32_t transition_sequence)
{
    voice_recording_critical_listener_slot_t
        listeners[VOICE_RECORDING_CRITICAL_MAX_LISTENERS] = {0};

    portENTER_CRITICAL(&s_lock);
    for (size_t index = 0U;
         index < VOICE_RECORDING_CRITICAL_MAX_LISTENERS;
         ++index) {
        listeners[index] = s_listeners[index];
    }
    portEXIT_CRITICAL(&s_lock);

    for (size_t index = 0U;
         index < VOICE_RECORDING_CRITICAL_MAX_LISTENERS;
         ++index) {
        if (listeners[index].listener != NULL) {
            listeners[index].listener(
                active,
                transition_sequence,
                listeners[index].context);
        }
    }
}

esp_err_t voice_recording_critical_enter(
    uint32_t session_generation,
    uint32_t ptt_generation)
{
    if ((session_generation == 0U) || (ptt_generation == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool notify = false;
    uint32_t transition_sequence = 0U;
    portENTER_CRITICAL(&s_lock);
    if (s_status.active) {
        const bool same_owner =
            (s_status.session_generation == session_generation) &&
            (s_status.ptt_generation == ptt_generation);
        portEXIT_CRITICAL(&s_lock);
        return same_owner ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_status.active = true;
    s_status.session_generation = session_generation;
    s_status.ptt_generation = ptt_generation;
    ++s_status.entered_count;
    ++s_status.transition_sequence;
    if (s_status.transition_sequence == 0U) {
        s_status.transition_sequence = 1U;
    }
    transition_sequence = s_status.transition_sequence;
    notify = true;
    portEXIT_CRITICAL(&s_lock);

    if (notify) {
        voice_recording_critical_notify_listeners(true, transition_sequence);
    }
    return ESP_OK;
}

esp_err_t voice_recording_critical_exit(
    uint32_t session_generation,
    uint32_t ptt_generation)
{
    if ((session_generation == 0U) || (ptt_generation == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool notify = false;
    uint32_t transition_sequence = 0U;
    portENTER_CRITICAL(&s_lock);
    if (!s_status.active) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    if ((s_status.session_generation != session_generation) ||
        (s_status.ptt_generation != ptt_generation)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.active = false;
    s_status.session_generation = 0U;
    s_status.ptt_generation = 0U;
    ++s_status.exited_count;
    ++s_status.transition_sequence;
    if (s_status.transition_sequence == 0U) {
        s_status.transition_sequence = 1U;
    }
    transition_sequence = s_status.transition_sequence;
    notify = true;
    portEXIT_CRITICAL(&s_lock);

    if (notify) {
        voice_recording_critical_notify_listeners(false, transition_sequence);
    }
    return ESP_OK;
}

void voice_recording_critical_get_status(
    voice_recording_critical_status_t *status)
{
    if (status == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

bool voice_recording_critical_is_active(void)
{
    bool active = false;
    portENTER_CRITICAL(&s_lock);
    active = s_status.active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

esp_err_t voice_recording_critical_register_listener(
    voice_recording_critical_listener_t listener,
    void *context)
{
    if (listener == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    for (size_t index = 0U;
         index < VOICE_RECORDING_CRITICAL_MAX_LISTENERS;
         ++index) {
        if ((s_listeners[index].listener == listener) &&
            (s_listeners[index].context == context)) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_OK;
        }
    }
    for (size_t index = 0U;
         index < VOICE_RECORDING_CRITICAL_MAX_LISTENERS;
         ++index) {
        if (s_listeners[index].listener == NULL) {
            s_listeners[index] = (voice_recording_critical_listener_slot_t) {
                .listener = listener,
                .context = context,
            };
            portEXIT_CRITICAL(&s_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_NO_MEM;
}
