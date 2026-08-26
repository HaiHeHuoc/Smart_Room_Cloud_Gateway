# Smart Room Cloud Gateway — AI Project State

Updated from branch: `test/phase14-ptt-voice-e2e-hil`
Snapshot date: 2026-08-26

## Working Constitution

- `AGENTS.md` is the repository-specific operating guide.
- Preserve completed roadmap history and phase boundaries.
- Inspect implementation/docs before editing.
- Keep changes evidence-based.
- Never claim build/HIL/merge/runtime success without evidence.
- `AI_Stored_Data/` is cross-session support metadata only and may be deleted; firmware/build code must never depend on it.

## Current high-level state

```text
Sprint 12  Software complete / HIL PASS 2026-08-25
Sprint 13  Software complete / HIL PASS 2026-08-25
Sprint 14  Software complete / Build PASS / golden-path HIL PASS
Sprint 15  NOT STARTED
```

A full architecture review through Phase 14 was completed on 2026-08-25 and is stored in `AI_Stored_Data/FULL_PROJECT_REVIEW_TO_PHASE14.md`.

## HIL routing

Phase 12:

- branch `test/xiaozhi-p2f-known-audio-e2e`;
- target acceptance **PASS / closed 2026-08-25**.

Phase 13:

- branch `test/phase13-voice-assistant-hil`;
- target acceptance **PASS / closed 2026-08-25**;
- lifecycle stress, recovery, connect-failure, coalescing, generation and resource evidence accepted.

Phase 14:

- production branch `phase/14-ptt-voice-mvp`;
- HIL plan `AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`;
- test branch `test/phase14-ptt-voice-e2e-hil`;
- golden-path physical PTT -> mic -> Xiaozhi -> response -> speaker ->
  repeated-turn evidence PASS on 2026-08-26; fault-injection cases remain
  SKIP/deferred.

## Sprint 14 — final software architecture

### User input

Current PTT hardware assignment:

```text
GPIO38 ---- push button ---- 3V3
internal pull-down
released LOW / pressed HIGH
```

GPIO38 is selected for PTT; GPIO48 remains reserved for the board's NeoPixel LED.

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

## Cross-system concurrency conclusion

Sensor, Firebase/cloud, GUI and other normal FreeRTOS workloads may continue to run while Xiaozhi is recording, sending or playing a response. The design does not require stopping unrelated tasks. Safety comes from explicit resource ownership and bounded queues/callbacks.

Important distinction:

```text
CPU/task interleaving                          allowed
Firebase + Xiaozhi network coexistence         allowed, HIL must measure contention
multiple clients directly accessing I2S        forbidden
multiple legitimate audio requests             require audio-manager arbitration
```

### Newly recorded architecture follow-up — general audio arbitration

`audio_manager` is currently the sole I2S owner, which prevents direct hardware ownership conflicts. However, there is not yet a complete general policy for cases such as:

- Xiaozhi speaking while a notification requests playback;
- Xiaozhi listening while another recorder requests microphone capture;
- a critical alarm arriving during Xiaozhi response playback.

Before intentionally enabling competing audio clients, add/review a centralized capture/playback arbitration policy in `audio_manager` (source/priority/interruptibility or an equivalent design). This is a post-Phase-14 architecture enhancement/integration requirement, not a reason to reopen the already scoped PTT MVP before its first HIL run.

Do not allow a new component to solve contention by calling I2S directly.

## Known Phase-14 acceptance boundaries

1. Golden-path target HIL and three repeated turns passed on GPIO38 with
   operator-confirmed audible response.
2. Stalled-response, network-loss, SD-unavailable and queue-pressure fault
   cases were not injected and remain SKIP/deferred.
3. Response playback is aggregated/SD-backed, not low-latency direct streaming.
4. The LCD `Starting...` route remains a Phase-15 UI concern.
5. `session_generation` identifies the long-lived session, not a unique PTT
   turn; turn serialization is part of the safety boundary.
6. Firebase/cloud + Xiaozhi simultaneous network/resource load has not yet
   been measured as a dedicated stress case on target hardware.
7. General multi-client audio arbitration is not yet implemented.

## Next-work guidance

When asked **"hiện tại nên làm gì tiếp theo?"**:

- State that Phase-12 and Phase-13 HIL are closed with PASS evidence.
- State that Phase-14 golden-path HIL passed and fault cases are deferred.
- Complete the Phase-14 Git checkpoint, then route the production commit into Phase 15 HIL/UI work.
- Mention the post-Phase-14 audio-arbitration/integration follow-up before enabling competing notification/alarm/recorder clients.
- If hardware is unavailable for a future rerun, preserve the recorded Phase-14 golden-path PASS and mark only new fault cases pending/deferred.
- Phase 14 software is complete.
- Do not start Sprint 15 implementation until the Phase-14 Git checkpoint is closed.

After Phase-14 closure, run Phase-15 voice/UI HIL, then a full-Gateway/Firebase integration regression using production branches only. The integration regression should include simultaneous Firebase/cloud and Xiaozhi traffic and any enabled competing audio-client scenarios.
