# Smart Room Cloud Gateway — AI Project State

Updated from branch: `phase/16-audio-arbitration`
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
Sprint 15  Software complete / Build + HIL pending
Sprint 16  IN PROGRESS / 16-A complete / 16-B next
```

A full architecture review through Phase 14 is stored in `AI_Stored_Data/FULL_PROJECT_REVIEW_TO_PHASE14.md`.
Phase-15 software closure is stored in `AI_Stored_Data/PHASE15_PROGRESS.md`.
Phase-16 active progress is stored in `AI_Stored_Data/PHASE16_PROGRESS.md`.

## Deferred HIL routing

Phase 12:

- branch `test/xiaozhi-p2f-known-audio-e2e`;
- activation `RUN PHASE 12 HIL`;
- known-audio E2E, BOOT Starting regression, real network-loss and resource evidence.

Phase 13:

- branch `test/phase13-voice-assistant-hil`;
- activation `RUN PHASE 13 HIL`;
- lifecycle, recovery, coalescing, stale/late event and resource evidence.

Phase 14:

- production branch `phase/14-ptt-voice-mvp`;
- plan `AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`;
- recommended test branch `test/phase14-ptt-voice-e2e-hil`;
- physical PTT -> mic -> Xiaozhi -> response -> speaker -> repeated-turn evidence pending.

Phase 15:

- production branch `phase/15-voice-assistant-ui`;
- test branch `test/phase15-voice-ui-hil`;
- plan `AI_Stored_Data/PHASE15_HIL_TEST_PLAN.md`;
- runbook `AI_Stored_Data/PHASE15_HIL_TEST_BRANCH.md` on the test branch;
- activation `RUN PHASE 15 HIL`;
- real USER/ASSISTANT semantic text, repeated latest-turn UX, recovery presentation and LVGL ownership evidence pending.

## Sprint 14 — voice/audio production architecture

Temporary PTT hardware reservation remains:

```text
GPIO5 ---- push button ---- 3V3
internal pull-down
released LOW / pressed HIGH
```

GPIO5 is intentionally temporary and must be rechecked/replaced later.
Factory reset remains independent on GPIO9.

Production voice path:

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
-> playback completion
-> next turn allowed
```

Ownership remains:

- `audio_manager`: sole microphone/speaker/I2S/DMA owner;
- `xiaozhi_foundation`: sole direct `esp_xiaozhi`/MCP/audio-channel boundary;
- `voice_assistant`: long-lived session/recovery orchestration;
- `voice_assistant_ptt`: user authorization policy;
- `app_gui`/`ui_manager_lvgl`: GUI/LVGL owner;
- `sd_card_manager`: SD lifecycle/lease owner.

## Sprint 15 — final software architecture

Production presentation path:

```text
voice_assistant lifecycle
          +
Xiaozhi CHAT_TEXT USER/ASSISTANT
          ↓
voice_assistant_ui_model
          ↓
voice_assistant_ui_gui_adapter
          ↓
app_gui copied queue
          ↓
app_gui UI task
          ↓
LVGL Voice/Xiaozhi screen
```

Production UI model contains bounded/copied exact lifecycle state, session generation, presentation-only `turn_sequence`, USER/ASSISTANT text, validity/truncation flags and last error. Xiaozhi text is callback-lifetime only and is copied before callback return. Stale session-generation text is rejected.

Latest-turn UX intentionally keeps only the current turn on the 160x128 display. New USER text increments `turn_sequence`, replaces USER text and clears the previous Assistant field; ASSISTANT text then fills the latest-turn response. A new Xiaozhi session clears both fields and resets `turn_sequence`.

Phase-15 final review fixed the legacy GUI recovery error-normalization issue in commit `d0d7a5b7b4fd1c52e163a917050420289ccd1684`.

MVP visual limitation remains accepted:

```text
CONNECTING -> PROCESSING
THINKING   -> PROCESSING
RECOVERING -> PROCESSING
READY      -> READY
LISTENING  -> LISTENING
SPEAKING   -> RESPONDING
ERROR      -> ERROR
```

Do not claim exact CONNECTING/THINKING/RECOVERING LCD labels.

## Sprint 16 — active audio arbitration architecture

Production branch: `phase/16-audio-arbitration`
Current checkpoint: **16-A complete / 16-B next**.

Phase 16 exists because sole I2S ownership is necessary but not sufficient when multiple legitimate clients request capture/playback concurrently.

16-A added the project-owned arbitration metadata contract:

```text
audio_manager_request_t
├── request_id
├── client
├── resource
├── priority
├── busy_policy
└── interruptible
```

Logical clients:

```text
SYSTEM
XIAOZHI
NOTIFICATION
ALARM
RECORDER
UI
TEST
```

Resource classes:

```text
CAPTURE
PLAYBACK
```

Busy-policy intent:

```text
REJECT
QUEUE
PREEMPT_LOWER_PRIORITY
```

Recommended priority defaults currently documented:

```text
CRITICAL_ALARM 100
SYSTEM          90
XIAOZHI         70
NOTIFICATION    50
UI              30
BACKGROUND      20
```

Important compatibility rule: 16-A did not change runtime behavior of the existing `audio_manager_start_recording()`, `audio_manager_stop_recording()`, `audio_manager_play_recorded()`, `audio_manager_play_wav()` or `audio_manager_stop_playback()` APIs. Runtime playback arbitration begins in 16-B; capture arbitration begins in 16-C.

No client gains direct I2S/DMA/file/raw-buffer ownership from the arbitration model.

## Production composition / validation isolation

The main composition starts the real audio manager first, then production voice/UI/PTT/uplink/downlink/GPIO stack. It does not auto-begin a Xiaozhi session at boot.

When `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=y`, the production Phase-14/15 voice stack is suppressed so validation does not intentionally compete with production voice/UI ownership.

## Cross-system concurrency conclusion

Sensor, Firebase/cloud, GUI and normal FreeRTOS tasks may continue while Xiaozhi records/sends/plays audio. Correctness depends on owned resources and bounded callbacks/queues rather than globally pausing unrelated tasks.

```text
CPU/task interleaving                          allowed
Firebase + Xiaozhi network coexistence         allowed, HIL must measure contention
multiple clients directly accessing I2S        forbidden
multiple legitimate audio-manager requests     centrally arbitrated by Phase 16
```

Current unresolved runtime cases Phase 16 will address include:

```text
Xiaozhi SPEAKING + notification playback
Xiaozhi SPEAKING + critical alarm
Xiaozhi LISTENING + another recorder request
```

## Known acceptance/build risks

1. Phase-14/15 build/link is not yet verified with target ESP-IDF toolchain.
2. Phase-14/15 target HIL has not run.
3. Actual Xiaozhi response audio codec must be proven; Opus evidence requires decoder work before Phase-14 speaker acceptance.
4. Phase-14 response playback is PSRAM/SD-backed rather than low-latency streaming.
5. GPIO5 PTT is temporary.
6. Firebase/cloud + Xiaozhi simultaneous load remains unmeasured.
7. Phase-15 source-scoped `esp_xiaozhi_chat_init` semantic bridge requires real build compatibility evidence against pinned `esp_xiaozhi` 0.1.2.
8. Phase-15 exact CONNECTING/THINKING/RECOVERING GUI labels are intentionally not implemented in the reused legacy visual enum.
9. Phase-16 arbitration runtime is not yet implemented beyond the 16-A metadata model.
10. Phase-16 build/HIL evidence does not exist yet.

## Next-work guidance

When asked **"hiện tại nên làm gì tiếp theo?"**:

- Always surface Phase-12 and Phase-13 Codex-ready HIL first.
- Mention Phase-14 HIL plan status.
- Mention Phase-15 HIL test branch `test/phase15-voice-ui-hil` and `RUN PHASE 15 HIL`.
- If hardware is unavailable, keep all HIL marked DEFERRED.
- Phase 14 software is complete.
- Phase 15 software is complete.
- Phase 16 is active; 16-A is complete.
- Next software checkpoint is **16-B — Playback Arbitration Runtime**, only after explicit `tiếp tục`.

When hardware is available, recommended order is Phase 12 HIL -> Phase 13 HIL -> Phase 14 PTT voice HIL -> Phase 15 voice UI HIL -> later Phase-16 arbitration HIL once software is closed -> full Gateway/Firebase integration regression using production branches only.

The full integration regression must include simultaneous Firebase/cloud and Xiaozhi traffic and enabled competing audio-client scenarios.
