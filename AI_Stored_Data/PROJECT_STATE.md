# Smart Room Cloud Gateway — AI Project State

Updated from branch: `phase/14-ptt-voice-mvp`
Snapshot date: 2026-08-25

## Working Constitution

- `AGENTS.md` is the repository-specific operating guide.
- Preserve completed roadmap history and phase boundaries.
- Inspect implementation/docs before editing.
- Keep changes evidence-based.
- Never claim build/HIL/merge/runtime success without evidence.
- `AI_Stored_Data/` is cross-session support metadata only and may be deleted; firmware/build code must never depend on it.

## Current high-level state

```text
Sprint 12  Software complete / selected HIL deferred
Sprint 13  Software complete / HIL deferred
Sprint 14  Software complete / Build + HIL pending
Sprint 15  NOT STARTED
```

## Deferred HIL routing

Phase 12:

- branch `test/xiaozhi-p2f-known-audio-e2e`;
- Codex activation `RUN PHASE 12 HIL`;
- known-audio E2E, BOOT Starting regression, real network-loss and resource evidence.

Phase 13:

- branch `test/phase13-voice-assistant-hil`;
- Codex activation `RUN PHASE 13 HIL`;
- lifecycle, recovery, coalescing, stale/late event and resource evidence.

Phase 14:

- production branch `phase/14-ptt-voice-mvp`;
- HIL plan `AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`;
- recommended future test branch `test/phase14-ptt-voice-e2e-hil`;
- full physical PTT -> mic -> Xiaozhi -> response -> speaker -> repeated-turn evidence still pending.

## Sprint 14 — final software architecture

### User input

Temporary PTT hardware reservation:

```text
GPIO5 ---- push button ---- 3V3
internal pull-down
released LOW / pressed HIGH
```

GPIO5 is intentionally temporary and will be changed later by Hải.

The factory-reset button remains independently owned on GPIO9 and is not reused for PTT.

### Production voice path

```text
voice_assistant_ptt_gpio
-> voice_assistant_ptt
-> voice_assistant / xiaozhi_foundation READY
-> audio_manager live PCM16 stream
-> voice_assistant_uplink bounded queue/task
-> Xiaozhi MANUAL audio uplink
-> release stops listening but retains response channel
-> Xiaozhi response callback
-> voice_assistant_downlink bounded queue + PSRAM aggregation
-> SD-managed PCM16 WAV
-> audio_manager_play_wav()
-> MAX98357 speaker
-> actual playback completion
-> next turn allowed
```

### Ownership

- `button_manager`: factory-reset GPIO9 only.
- `voice_assistant_ptt_gpio`: dedicated PTT GPIO polling/debounce only.
- `voice_assistant_ptt`: authorization policy only.
- `audio_manager`: sole microphone/speaker/I2S/DMA owner.
- `audio_manager_stream/tap`: copied live PCM publication only.
- `voice_assistant_uplink`: copied bounded mic transport queue/coordination.
- `voice_assistant_downlink`: copied response aggregation and audio-manager playback request.
- `xiaozhi_foundation`: sole direct `esp_xiaozhi`/MCP/audio-channel boundary.
- `voice_assistant`: long-lived session/generation/recovery orchestration.
- `app_gui`/`ui_manager_lvgl`: GUI/LVGL ownership.

### Production composition

`main.c` startup logic is preserved. `main/CMakeLists.txt` source-scopes the main audio registration/start calls through `main/phase14_voice_composition.c`.

The wrapper:

- preserves existing GUI audio callback;
- fans copied audio status into the voice adapter;
- starts the real audio manager first;
- then initializes/starts voice, PTT policy, uplink, downlink and PTT GPIO input;
- does not auto-begin a Xiaozhi session at boot;
- suppresses Phase-14 production voice when Phase-12 Xiaozhi validation mode is explicitly enabled.

## Sprint 14 robustness

- release-before-READY cannot authorize capture;
- callbacks remain non-blocking/copy-only;
- uplink/downlink queues are bounded;
- queue loss is diagnostic and corrupt downlink is not falsely played as success;
- response inactivity timeout is 15 s;
- audio-idle wait is bounded to 10 s;
- response playback completion wait is bounded to 60 s;
- repeated turns are serialized against prior downlink/playback;
- transport recovery remains owned by Phase-13 voice-assistant recovery;
- no unbounded reconnect loop;
- SD failure remains under `sd_card_manager` ownership.

## Known Phase-14 acceptance risks

1. Build/link not yet verified with the target ESP-IDF toolchain.
2. Target HIL not yet run.
3. Actual Xiaozhi response codec must be proven. The MVP currently assumes PCM16-compatible downlink after PCM negotiation; Opus evidence requires adding a decoder before speaker acceptance.
4. Response playback is aggregated/SD-backed, not low-latency direct streaming.
5. GPIO5 is temporary.
6. `session_generation` identifies the long-lived session, not a unique PTT turn; turn serialization is part of the safety boundary.

## Next-work guidance

When asked **"hiện tại nên làm gì tiếp theo?"**:

- Always mention Phase-12 and Phase-13 Codex-ready HIL backlogs.
- Also mention Phase-14 HIL plan/test-branch pending status.
- If hardware is unavailable, keep all HIL marked DEFERRED.
- Phase 14 software is complete.
- Do not start Sprint 15 automatically; wait for explicit user direction.

When hardware becomes available, recommended order is Phase 12 HIL -> Phase 13 HIL -> Phase 14 full PTT voice HIL, then full-Gateway/Firebase integration regression using production branches only.
