# Phase 14 Push-To-Talk Voice MVP Progress

Updated: 2026-08-25
Branch: `phase/14-ptt-voice-mvp`
Current checkpoint: **14-A — PTT Trigger + Authorization State**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 14 is implemented in reviewable parts. After each part, stop coding and wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

Planned checkpoints:

1. 14-A — PTT trigger + authorization state.
2. 14-B — microphone/public streaming contract.
3. 14-C — Xiaozhi audio uplink.
4. 14-D — response downlink + speaker playback.
5. 14-E — cancel/recovery/repeated-turn robustness.
6. 14-F — FINAL Phase-14 review/docs/deferred HIL.

When 14-F is reached, explicitly notify Hải that it is the final Phase-14 prompt before software closure.

## 14-A — implemented

Added the project-owned PTT policy surface inside `voice_assistant`:

- `include/voice_assistant_ptt.h`;
- `voice_assistant_ptt.c`;
- bounded PTT task/queue/mutex;
- `voice_assistant_ptt_press()`;
- `voice_assistant_ptt_release()`;
- `voice_assistant_ptt_cancel()`;
- copied PTT status observer/getter;
- independent non-zero PTT generation;
- copied voice-session generation tracking;
- explicit `capture_authorized` boolean.

### PTT states

```text
UNINITIALIZED
IDLE
ARMING_SESSION
AUTHORIZED
RELEASED
CANCEL_PENDING
ERROR
```

### Authorization rule

Microphone transmission authorization is never granted merely because a button/command was pressed.

```text
PTT press
-> if voice session IDLE: request begin_session()
-> ARMING_SESSION, authorized=false
-> wait asynchronously for real voice_assistant READY
-> only while press is still held: AUTHORIZED, authorized=true
```

If PTT is released before READY:

```text
ARMING_SESSION
-> release
-> CANCEL_PENDING, authorized=false
-> wait for bounded transport start to resolve
-> if READY appears, immediately request end_session()
-> IDLE after cleanup
```

This prevents microphone authorization after a release that occurred during connection setup.

If PTT is released after authorization:

```text
AUTHORIZED
-> RELEASED
capture_authorized=false
```

14-A deliberately does not start or stop microphone capture. Phase 14-B will consume this authorization contract.

### Cancel rule

Explicit cancel always revokes authorization. If the Xiaozhi start operation is still CONNECTING, cancel remains pending instead of attempting an unsafe/preemptive upstream teardown. Once READY/IDLE/error becomes observable, the policy reconciles deterministically.

### Temporary physical PTT GPIO reservation

`board_config.h` now reserves **GPIO5** as the temporary dedicated PTT input for Phase 14.

Temporary wiring/polarity:

```text
GPIO5 ---- push button ---- 3V3
```

The future input implementation must configure the ESP32-S3 internal **pull-down**:

```text
released = LOW
pressed  = HIGH
```

Board macros:

```c
#define PTT_BUTTON_GPIO                  GPIO_NUM_5
#define PTT_BUTTON_ACTIVE_LEVEL          1
#define PTT_BUTTON_USE_INTERNAL_PULLDOWN 1
#define PTT_BUTTON_POLL_PERIOD_MS        10U
#define PTT_BUTTON_DEBOUNCE_MS           40U
```

**GPIO5 is temporary.** Hải explicitly plans to change the final PTT GPIO later. Re-check the complete hardware/pin map before considering the hardware assignment stable. The current change only reserves/configures the board-level contract; no GPIO driver is bound to `voice_assistant_ptt` yet.

### Input ownership decision

The existing physical `button_manager` is already the factory-reset input and supports one application callback. Sprint 14 roadmap also requires that PTT not overload the factory-reset long press.

Therefore 14-A does **not** bind PTT to the current reset button. The PTT API is input-source agnostic; a later dedicated physical/button/UI trigger may call it through application composition without moving reset ownership.

### Ownership preserved

- `button_manager`: reset-button sampling only; no PTT ownership added.
- `audio_manager`: sole I2S/DMA/PCM owner; untouched in 14-A.
- `xiaozhi_foundation`: Xiaozhi transport/session boundary; untouched in 14-A.
- `voice_assistant`: session orchestration from Phase 13.
- `voice_assistant_ptt`: user authorization policy only; no GPIO/I2S/LVGL ownership.

### Timing / bounded behavior

- PTT task stack: 4096 bytes.
- PTT task priority: 4.
- queue length: 6.
- lock wait: 100 ms.
- task-start wait: 2 s.
- asynchronous session arming timeout: 20 s.
- reconciliation poll: 50 ms.
- one public PTT command pending at a time.

## Not implemented in 14-A

- no PTT GPIO driver/binding yet (GPIO5 is only reserved in board config);
- no factory-reset button reuse;
- no microphone capture;
- no live PCM frame/ring export;
- no Opus/audio-channel uplink;
- no response audio playback;
- no automatic LISTENING/THINKING/SPEAKING transition;
- no production `main.c` PTT trigger;
- no hardware/build acceptance.

## Static review notes

1. `capture_authorized=true` requires real `voice_assistant` READY evidence.
2. release-before-READY was hardened to `CANCEL_PENDING`; it cannot later authorize capture.
3. callback publication occurs after the PTT mutex is released.
4. PTT task never calls GPIO, I2S, LVGL, or private Xiaozhi APIs.
5. a press from `RELEASED` may authorize a later turn on an already-READY session; bounded multi-turn policy is finalized later in Phase 14.
6. GPIO5 is a temporary reservation and must be replaceable without changing PTT policy code.
7. no ESP-IDF build/HIL PASS is claimed in this session.

## Next checkpoint — only after user says `tiếp tục`

**14-B — Microphone / Public Streaming Contract**

Planned scope:

1. inspect current `audio_manager` capture internals and accepted ownership boundaries;
2. add a bounded public live PCM delivery contract without exposing I2S/DMA/private buffers;
3. start capture only while PTT `capture_authorized=true`;
4. revoke/stop capture on release/cancel/error;
5. preserve 16-kHz mono PCM baseline and audio-manager sole I2S ownership;
6. do not yet implement Xiaozhi Opus uplink unless the contract boundary is complete.
