# Phase 13 Voice Assistant Progress

Updated: 2026-08-25
Branch: `phase/13-voice-assistant`
Current checkpoint: **13-B — Xiaozhi Production Integration**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 13 is implemented in reviewable parts. After each part, stop coding and wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

## 13-A — completed

Created `components/application/voice_assistant/` with:

- `CMakeLists.txt`;
- `include/voice_assistant.h`;
- `voice_assistant.c`;
- `README.md`.

Implemented:

- one long-lived orchestration task;
- 4096-byte stack, priority 4;
- bounded queue length 8;
- bounded status mutex waits;
- 2-second task-start readiness wait;
- copied status observer called only after releasing the mutex;
- non-zero session generation;
- stale queued command rejection;
- conversation states `UNINITIALIZED`, `INITIALIZED`, `IDLE`, `CONNECTING`, `READY`, `LISTENING`, `THINKING`, `SPEAKING`, `RECOVERING`, `ERROR`.

## 13-B — implemented

### New production Xiaozhi lifecycle surface

Added `components/application/xiaozhi_foundation/xiaozhi_session.c` and public session APIs:

- `xiaozhi_foundation_session_register_status_callback()`;
- `xiaozhi_foundation_session_start(client_generation)`;
- `xiaozhi_foundation_session_stop()`;
- `xiaozhi_foundation_session_get_status()`;
- `xiaozhi_foundation_session_state_to_string()`.

This path is distinct from the Phase-12 P2-E/P2-F validation worker. It keeps a successful WebSocket chat session alive until explicit stop or transport failure instead of validating and immediately tearing down.

### Production session start flow

```text
voice_assistant begin command
-> CONNECTING
-> xiaozhi_foundation_session_start(generation)
-> esp_xiaozhi_chat_get_info()
-> require WebSocket capability
-> create MCP
-> create EventGroup
-> chat init (WebSocket only, MQTT preference disabled)
-> register ESP_XIAOZHI_CHAT_EVENTS handler
-> chat start
-> wait <= 15 s for real CONNECTED
-> READY
```

A successful return therefore represents real transport connection evidence; READY is not synthesized.

### Production session cleanup flow

```text
end command
-> chat stop
-> event handler unregister
-> chat deinit
-> MCP destroy
-> EventGroup delete
-> IDLE
```

The new session path suppresses the same upstream INFO log tags that can emit raw Xiaozhi/MCP payload data and restores them only after cleanup.

### Voice-assistant integration

`voice_assistant` now depends only on the public `xiaozhi_foundation` session API, never directly on `esp_xiaozhi` or MCP types.

Foundation callback flow:

```text
Xiaozhi event loop
-> copied foundation session status
-> non-blocking voice_assistant queue send
-> voice_assistant task
-> generation check
-> READY or ERROR transition
```

The foundation copies the application-provided `client_generation` into each status snapshot. Late events with a generation different from the current conversation are dropped.

### Ownership preserved

13-B still does **not**:

- access I2S;
- call `audio_manager`;
- open the production Xiaozhi audio channel;
- stream microphone/audio data;
- route response audio;
- call LVGL or `app_gui`;
- own Wi-Fi/provisioning/cloud/storage;
- expose Xiaozhi/MCP handles, tokens, endpoints, or framework-owned pointers.

`main.c` is intentionally still not changed to initialize/start `voice_assistant`. Production composition and UI/audio contracts are handled in later Phase-13 checkpoints.

## Static review notes

- READY is gated by the real `ESP_XIAOZHI_CHAT_EVENT_CONNECTED` event.
- Xiaozhi callbacks do not directly mutate voice state; they copy/queue events.
- The long-lived production session follows the already validated Phase-12 cleanup order.
- A same-generation READY callback can be processed after the synchronous session-start confirmation, producing an idempotent `READY -> READY` diagnostic transition. This is acceptable for 13-B; dedupe/robustness can be handled in 13-D if useful.
- Phase-12 validation lifecycle and Phase-13 production session lifecycle are separate surfaces. Application composition must not invoke them concurrently; explicit cross-surface exclusion is deferred to the robustness checkpoint.
- Intentional-stop/disconnect ordering and queued late-error handling should be part of 13-D software robustness review before Phase-13 closure.

No ESP-IDF build or target HIL is claimed because this environment did not run the project toolchain/board.

## Next checkpoint — only after user says `tiếp tục`

**13-C — Audio + GUI Contracts**

Planned scope:

1. inspect existing `audio_manager` manual-record/playback contracts and `app_gui` queues;
2. define the production audio data/control boundary without transferring I2S ownership;
3. promote required Xiaozhi conversation events (transcript/audio-state facts) through copied project-owned types;
4. connect `voice_assistant` status to the GUI queue without calling LVGL outside the UI task;
5. prepare LISTENING / THINKING / SPEAKING transitions;
6. do not claim real mic/speaker behavior without HIL.
