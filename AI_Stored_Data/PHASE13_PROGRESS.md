# Phase 13 Voice Assistant Progress

Updated: 2026-08-25
Branch: `phase/13-voice-assistant`
Current checkpoint: **13-A — Foundation + State Machine**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 13 is being implemented in reviewable parts. After each part, stop coding
and wait for Hải to review. Continue only after the user explicitly says
`tiếp tục`.

## 13-A implemented

Created `components/application/voice_assistant/` with:

- `CMakeLists.txt`;
- `include/voice_assistant.h`;
- `voice_assistant.c`;
- `README.md`.

The component is added to the main build dependency graph, but `main.c` does not
initialize/start it yet. This intentionally compiles the component without
changing current Gateway runtime composition before Phase 13-B integration.

### Public contract

- `voice_assistant_init()`;
- `voice_assistant_start()`;
- `voice_assistant_begin_session()`;
- `voice_assistant_end_session()`;
- `voice_assistant_register_status_callback()`;
- `voice_assistant_get_status()`;
- `voice_assistant_state_to_string()`.

### State model

Defined states:

`UNINITIALIZED -> INITIALIZED -> IDLE -> CONNECTING`

and reserved production-facing states:

`READY`, `LISTENING`, `THINKING`, `SPEAKING`, `RECOVERING`, `ERROR`.

Phase 13-A deliberately stops a started conversation at `CONNECTING`; it does
not fake `READY`. Phase 13-B must supply real Xiaozhi transport evidence for
that transition.

### Concurrency/ownership foundation

- one long-lived `voice_assistant` orchestration task;
- 4096-byte task stack;
- priority 4;
- bounded command queue length 8;
- bounded status mutex wait;
- 2-second task-start readiness wait;
- status callbacks run only after releasing the component mutex;
- one in-flight public session command at a time;
- non-zero monotonically advancing session generation;
- queued commands carry generation and stale generations are dropped.

### Ownership preserved

13-A does **not**:

- access I2S;
- call `audio_manager`;
- call `xiaozhi_foundation` or `esp_xiaozhi`;
- call LVGL or `app_gui`;
- own Wi-Fi/provisioning/cloud/storage;
- synthesize transport success.

## Static review notes

Current branch diff from the Phase 12 base is limited to the new
`voice_assistant` component plus one `main/CMakeLists.txt` dependency line.
`main.c` is intentionally unchanged.

No build or hardware acceptance is claimed in this session because no local
ESP-IDF build/HIL execution was performed through the available environment.

## Next checkpoint — only after user says `tiếp tục`

**13-B — Xiaozhi production integration**

Planned scope:

1. inspect current public `xiaozhi_foundation` validation boundary;
2. separate reusable production lifecycle/status API from Phase-12 validation;
3. connect Xiaozhi events to the `voice_assistant` task through copied bounded
   events carrying session generation;
4. implement real `CONNECTING -> READY` and transport-failure transitions;
5. begin moving temporary Xiaozhi orchestration out of `main.c` only where the
   new component fully replaces it;
6. do not integrate microphone/speaker streaming yet.
