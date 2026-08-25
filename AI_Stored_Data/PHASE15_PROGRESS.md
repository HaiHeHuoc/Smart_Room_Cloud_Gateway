# Phase 15 Voice Assistant UI / Conversation Presentation Progress

Updated: 2026-08-25
Branch: `phase/15-voice-assistant-ui`
Current checkpoint: **15-C — Production USER Transcript Wiring**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 15 follows the same review cadence as Phases 13 and 14. Implement one checkpoint, stop, explain what changed, and continue only when Hải says `tiếp tục`.

Planned checkpoints:

1. 15-A — production voice event/UI model. ✅
2. 15-B — dedicated Voice Assistant screen and lifecycle presentation. ✅
3. 15-C — production USER transcript wiring. ✅
4. 15-D — production ASSISTANT text wiring. NEXT
5. 15-E — repeated-turn/history/error UX + presentation refinement.
6. 15-F — FINAL review/composition/docs/deferred HIL.

At 15-F explicitly notify Hải that it is the final Phase-15 prompt before software closure.

## 15-A summary

Added the production `voice_assistant_ui_model` above `voice_assistant` with copied lifecycle state, session generation, bounded 192-byte user/assistant text buffers, validity/truncation flags, stale-generation rejection and stale-text clearing on a new non-zero session generation.

## 15-B summary

Added `voice_assistant_ui_gui_adapter` and reused/promoted the existing UI-task-owned Xiaozhi visual surface. Production presentation now flows:

```text
voice_assistant
-> voice_assistant_ui_model
-> voice_assistant_ui_gui_adapter
-> app_gui latest-value queue
-> app_gui UI task
-> LVGL
```

No LVGL API is called from voice/Xiaozhi callbacks.

Current visual mapping limitation remains explicit: CONNECTING/THINKING/RECOVERING currently render through legacy PROCESSING until 15-E refinement.

## 15-C — implemented

### Exact semantic event contract

The project lock currently pins `espressif/esp_xiaozhi` **0.1.2**. The production semantic bridge is written against the component API contract in which:

```c
ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT
```

provides borrowed `esp_xiaozhi_chat_text_data_t *` with roles:

```c
ESP_XIAOZHI_CHAT_TEXT_ROLE_USER
ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT
```

The text pointer is treated as callback-lifetime only and is never retained by the project boundary.

Build verification against the installed/pinned 0.1.2 headers is still required before claiming compatibility.

### Production semantic-text boundary

Added to `xiaozhi_foundation`:

```text
xiaozhi_foundation_text_role_t
xiaozhi_foundation_text_event_t
xiaozhi_foundation_text_callback_t
xiaozhi_foundation_text_register_callback()
```

This is separate from both:

- Phase-14 binary/TTS response callback used by downlink;
- temporary Phase-12 validation UI status.

The text event carries only:

```text
role
client_generation
borrowed text pointer
```

Consumers must copy before returning.

### Source-local Xiaozhi init bridge

Added:

`components/application/xiaozhi_foundation/xiaozhi_text_bridge.c`

Only `xiaozhi_session.c` has its `esp_xiaozhi_chat_init()` symbol redirected at compile time to:

```c
xiaozhi_foundation_chat_init_bridge()
```

The bridge:

1. copies the existing Xiaozhi config;
2. preserves the original production protocol callback and context;
3. installs a project bridge callback;
4. forwards every event to the original callback unchanged;
5. additionally promotes `CHAT_TEXT` as project semantic text;
6. calls the real `esp_xiaozhi_chat_init()` from its own translation unit.

This avoids rewriting the large production session lifecycle while keeping the semantic hook scoped only to the production session source.

### USER transcript path

`voice_assistant_ui_model_start()` now registers the semantic text observer.

Current 15-C handling:

```text
Xiaozhi CHAT_TEXT
    ↓
role == USER
    ↓
xiaozhi_foundation text bridge
    ↓
client/session generation snapshot
    ↓
voice_assistant_ui_model_post_user_text()
    ↓
bounded 192-byte copy
    ↓
voice_assistant_ui_gui_adapter
    ↓
app_gui queue
    ↓
UI task / LVGL
```

ASSISTANT-role text is already classified by the foundation bridge but intentionally ignored by the UI model until 15-D.

### Lifetime and stale-event rules

- Xiaozhi `event_data` and `text` are borrowed only during the upstream callback.
- `xiaozhi_foundation` does not retain the text pointer.
- `voice_assistant_ui_model_post_user_text()` copies synchronously before the callback returns.
- the UI model rejects a text event whose `client_generation` does not match its current production voice session.
- new session generation clears old user/assistant text.
- app_gui receives only copied fixed-size text fields.

### Callback ownership

The project now deliberately keeps distinct observer paths:

```text
xiaozhi session status callback
    -> voice_assistant

xiaozhi binary/TTS response callback
    -> voice_assistant_downlink

xiaozhi semantic text callback
    -> voice_assistant_ui_model
```

This prevents USER transcript work from stealing the response callback already owned by downlink.

## Static review notes

1. semantic bridge forwards the original protocol callback before additional text publication, so existing Phase-13/14 TTS/error handling remains in place.
2. no network, LVGL or speaker operation runs from the text callback.
3. semantic callback executes outside the bridge critical section.
4. generation is copied from the current project session snapshot.
5. UI text is copied before callback return and remains bounded/null-terminated.
6. ASSISTANT-role semantic classification exists at the foundation boundary but no assistant UI update is performed yet.
7. source-scoped init redirect is another controlled CMake integration seam and must be verified by a real ESP-IDF build; it should be refactored later if it proves fragile.
8. no ESP-IDF build or HIL PASS is claimed.

## Not implemented yet

- no production ASSISTANT text presentation yet;
- no multi-turn conversation history;
- no exact production visual enum for CONNECTING vs THINKING vs RECOVERING;
- no waveform/audio-level visualization;
- no GUI PTT button;
- no new hardware behavior.

## Next checkpoint — only after user says `tiếp tục`

**15-D — Production ASSISTANT Text Wiring**

Planned scope:

1. consume the already-promoted semantic role ASSISTANT;
2. copy assistant text through `voice_assistant_ui_model_post_assistant_text()`;
3. preserve generation/lifetime/truncation rules;
4. confirm the 15-B GUI adapter presents the assistant text independently of response audio playback;
5. do not implement history/state refinement until 15-E.
