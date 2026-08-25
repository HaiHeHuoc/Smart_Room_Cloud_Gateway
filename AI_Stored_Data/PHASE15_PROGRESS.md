# Phase 15 Voice Assistant UI / Conversation Presentation Progress

Updated: 2026-08-25
Branch: `phase/15-voice-assistant-ui`
Current checkpoint: **15-A — Production Voice Event/UI Model**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 15 follows the same review cadence as Phases 13 and 14. Implement one checkpoint, stop, explain what changed, and continue only when Hải says `tiếp tục`.

Planned checkpoints:

1. 15-A — production voice event/UI model. ✅
2. 15-B — dedicated Voice Assistant screen and lifecycle presentation.
3. 15-C — production USER transcript wiring.
4. 15-D — production ASSISTANT text wiring.
5. 15-E — repeated-turn/history/error UX.
6. 15-F — FINAL review/composition/docs/deferred HIL.

At 15-F explicitly notify Hải that it is the final Phase-15 prompt before software closure.

## 15-A — implemented

Added a production presentation model inside `voice_assistant`:

- `include/voice_assistant_ui_model.h`;
- `voice_assistant_ui_model.c`;
- bounded 192-byte user transcript buffer;
- bounded 192-byte assistant response buffer;
- copied lifecycle state;
- copied session generation;
- copied last error;
- valid/truncated flags;
- single copied observer callback;
- copied getter;
- explicit user/assistant text ingress APIs;
- generation mismatch rejection;
- automatic stale-text clearing when a new non-zero session generation appears.

### Presentation states

```text
IDLE
CONNECTING
READY
LISTENING
THINKING
SPEAKING
RECOVERING
ERROR
```

15-A maps these from the existing `voice_assistant_state_t`. It does not invent new transport ownership and does not call LVGL.

### Text contract

Production UI text is project-owned and bounded:

```c
#define VOICE_ASSISTANT_UI_TEXT_BUFFER_SIZE 192U
```

Ingress:

```c
voice_assistant_ui_model_post_user_text(generation, text);
voice_assistant_ui_model_post_assistant_text(generation, text);
```

The caller keeps ownership of the input string; the model copies it before returning. Text from a stale or mismatched session generation is rejected.

### Why this is not the old Phase-12 UI model

`xiaozhi_foundation_ui_status_t` remains a temporary Phase-12 validation presentation structure. Phase 15 introduces a separate production model above `voice_assistant` so production GUI does not depend on validation-only state/types.

### Production composition

The existing Phase-14 production composition now initializes/starts the production UI model after `voice_assistant_start()` and before PTT/uplink/downlink startup.

```text
audio_manager READY
-> voice_assistant init/start
-> voice_assistant_ui_model init/start
-> PTT policy
-> uplink
-> downlink
-> PTT GPIO
```

The model currently observes copied `voice_assistant` lifecycle state through the existing status callback. No GUI adapter is registered yet; that is 15-B.

## Upstream Xiaozhi semantic evidence

The pinned/current `esp_xiaozhi` protocol API exposes `ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT` with `esp_xiaozhi_chat_text_data_t { role, text }`, where role distinguishes USER and ASSISTANT. The production `xiaozhi_session.c` currently promotes TTS/audio/error events but not CHAT_TEXT.

Therefore Phase 15 will wire text in two dedicated checkpoints instead of reusing/parsing validation logs:

- 15-C: CHAT_TEXT role USER -> `voice_assistant_ui_model_post_user_text()`;
- 15-D: CHAT_TEXT role ASSISTANT -> `voice_assistant_ui_model_post_assistant_text()`.

No semantic text is claimed to reach the production UI yet.

## Static review notes

1. UI model callback is invoked after releasing its mutex.
2. No LVGL API is called from `voice_assistant_ui_model`.
3. Text buffers are always null terminated.
4. text >= 192 bytes is truncated with an explicit flag.
5. stale generation text is rejected.
6. new session generation clears old user/assistant text to prevent stale conversation display.
7. `AI_Stored_Data` remains documentation-only and is not a firmware dependency.
8. No ESP-IDF build or HIL PASS is claimed.

## Not implemented in 15-A

- no dedicated LVGL Voice Assistant screen;
- no `app_gui` production voice queue/adapter;
- no real USER transcript ingress yet;
- no real ASSISTANT text ingress yet;
- no conversation history;
- no waveform/audio-level visualization;
- no GUI PTT button;
- no new hardware behavior.

## Next checkpoint — only after user says `tiếp tục`

**15-B — Dedicated Voice Assistant Screen / Lifecycle Presentation**

Planned scope:

1. add production voice UI status type/queue to `app_gui`;
2. register an application adapter from `voice_assistant_ui_model` to `app_gui`;
3. render `IDLE / CONNECTING / READY / LISTENING / THINKING / SPEAKING / RECOVERING / ERROR` only inside the UI task;
4. reserve bounded areas for user/assistant text without wiring real text events yet;
5. preserve all existing Wi-Fi/sensor/cloud screens and LVGL ownership.
