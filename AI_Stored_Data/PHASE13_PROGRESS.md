# Phase 13 Voice Assistant Progress

Updated: 2026-08-25
Branch: `phase/13-voice-assistant`
Current checkpoint: **13-C — Audio + GUI Contracts**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 13 is implemented in reviewable parts. After each part, stop coding and wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

## 13-A — completed

Created `components/application/voice_assistant/` and implemented:

- one long-lived orchestration task;
- 4096-byte stack, priority 4;
- bounded queue length 8;
- bounded status mutex waits;
- 2-second task-start readiness wait;
- copied status observer called only after releasing the mutex;
- non-zero session generation;
- stale queued command rejection;
- conversation states `UNINITIALIZED`, `INITIALIZED`, `IDLE`, `CONNECTING`, `READY`, `LISTENING`, `THINKING`, `SPEAKING`, `RECOVERING`, `ERROR`.

## 13-B — completed

### Production Xiaozhi lifecycle surface

Added `components/application/xiaozhi_foundation/xiaozhi_session.c` and public session APIs:

- `xiaozhi_foundation_session_register_status_callback()`;
- `xiaozhi_foundation_session_start(client_generation)`;
- `xiaozhi_foundation_session_stop()`;
- `xiaozhi_foundation_session_get_status()`;
- `xiaozhi_foundation_session_state_to_string()`.

The production session is separate from the Phase-12 P2-E/P2-F validation worker. A successful session start requires a real WebSocket CONNECTED event and the session stays alive until explicit stop or transport failure.

Foundation callback flow:

```text
Xiaozhi event loop
-> copied foundation session status
-> non-blocking voice_assistant queue send
-> voice_assistant task
-> generation check
-> READY or ERROR transition
```

No Xiaozhi/MCP handles, tokens, endpoints, credentials, or framework-owned pointers cross the public boundary.

## 13-C — implemented

### Audio contract

Added a project-owned audio status model to `voice_assistant.h`:

- `voice_assistant_audio_state_t`;
- `voice_assistant_audio_status_t`;
- copied audio state inside `voice_assistant_status_t`;
- `voice_assistant_notify_audio_status()`;
- `voice_assistant_audio_state_to_string()`.

The voice task now accepts bounded copied audio-status commands and publishes them through the same UI-safe status callback. Audio status and conversation state remain distinct.

### audio_manager fan-out adapter

Added:

- `include/voice_assistant_audio_adapter.h`;
- `voice_assistant_audio_adapter.c`.

The adapter converts one existing `audio_manager_status_t` snapshot into project-owned voice audio facts and calls `voice_assistant_notify_audio_status()`.

It deliberately does **not** register a second callback with `audio_manager`. Application composition can fan out its existing audio callback to both GUI and voice-assistant adapters later.

Copied fields only:

- mapped lifecycle state;
- `capture_i2s_active`;
- `playback_i2s_active`;
- `last_error`.

No I2S handle, DMA descriptor, PCM buffer, private recording storage, or hardware callback crosses the adapter.

### Transitional snapshot rule

The audio contract treats lifecycle state and I2S-active flags as independent transition facts. It rejects only impossible simultaneous capture+playback, rather than requiring every RECORDING snapshot to already have capture-active=true or every PLAYBACK snapshot to already have playback-active=true. This avoids dropping legitimate transitional status snapshots.

### GUI contract

`voice_assistant_status_t` is now explicitly the production UI-safe voice model. It contains only copied project-owned scalar state:

```text
conversation state
session generation
session-active flag
last voice error
copied audio state
capture/playback active flags
last audio error
```

`voice_assistant` still has no dependency on `app_gui` or LVGL. A future application/UI adapter may copy this status into the app_gui task. The temporary Phase-12 `ui_xiaozhi_status_t` is intentionally not reused as the production contract.

### Important audio limitation discovered

Current `audio_manager` does **not** expose a public live PCM frame/ring interface. Its production public surface owns recording/playback internally and retains processed recordings privately.

Therefore 13-C intentionally does not bypass private audio storage or introduce a second I2S owner. Real mic -> Xiaozhi streaming requires a bounded public streaming contract in a later integration step while `audio_manager` remains the sole I2S/DMA owner.

### State semantics preserved

Generic audio activity does **not** automatically drive:

- `LISTENING`;
- `THINKING`;
- `SPEAKING`.

A local recording or WAV playback may be unrelated to the voice conversation. Those transitions are reserved until `voice_assistant` explicitly owns the voice command flow.

## Ownership after 13-C

```text
audio_manager
    -> sole microphone/speaker/I2S/DMA/PCM owner

xiaozhi_foundation
    -> Xiaozhi transport/session boundary

voice_assistant
    -> conversation/session state + copied audio/transport orchestration

app_gui
    -> sole GUI/LVGL owner
```

## Static review notes

- Audio notifications are non-blocking queue operations.
- Voice status callbacks execute after the voice status mutex is released.
- No audio callback directly changes conversation state.
- No direct LVGL/app_gui call was introduced.
- No raw PCM or private audio-manager buffer was exposed.
- `READY -> READY` is deduplicated in the foundation-status handler when a same-generation asynchronous READY follows synchronous start confirmation.
- Phase-12 validation and Phase-13 production session are still separate lifecycle surfaces; cross-surface exclusion remains a 13-D robustness item.
- Intentional-stop/disconnect ordering, abort while CONNECTING, queue-pressure policy, recovery, and repeated-session robustness remain 13-D items.

No ESP-IDF build or target HIL is claimed because this environment did not run the project toolchain/board.

## Next checkpoint — only after user says `tiếp tục`

**13-D — Failure / Recovery + Software Validation**

Planned scope:

1. harden cross-surface exclusion between Phase-12 validation and Phase-13 production session;
2. define bounded recovery/error policy for transport loss;
3. review begin/end/stop ordering, including cancellation while CONNECTING;
4. harden stale/late callback handling after intentional stop;
5. exercise queue-pressure and duplicate-command behavior by software/static validation hooks where useful;
6. review repeated session start/stop lifecycle and cleanup ownership;
7. prepare deferred HIL procedures/log markers without claiming target PASS;
8. do not implement Phase-14 real microphone streaming just to close Phase 13.
