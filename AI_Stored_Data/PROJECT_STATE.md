# Smart Room Cloud Gateway — AI Project State

Updated from branch: `phase/15-voice-assistant-ui`
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
Sprint 16  NOT STARTED
```

A full architecture review through Phase 14 is stored in `AI_Stored_Data/FULL_PROJECT_REVIEW_TO_PHASE14.md`.
Phase-15 software closure is stored in `AI_Stored_Data/PHASE15_PROGRESS.md`.

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
- plan `AI_Stored_Data/PHASE15_HIL_TEST_PLAN.md`;
- recommended test branch `test/phase15-voice-ui-hil`;
- suggested activation `RUN PHASE 15 HIL`;
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

### Production UI model

Contains bounded/copied:

- exact production lifecycle state;
- session generation;
- presentation-only `turn_sequence`;
- 192-byte USER text;
- 192-byte ASSISTANT text;
- validity/truncation flags;
- last error.

Text from Xiaozhi is callback-lifetime only and is copied before callback return. Stale session-generation text is rejected.

### Latest-turn UX

The 160x128 LCD intentionally keeps only the latest conversation turn:

```text
new USER semantic text
-> increment turn_sequence
-> clear previous ASSISTANT text
-> replace USER text

ASSISTANT semantic text
-> retain USER text
-> replace ASSISTANT text
```

New Xiaozhi session generation clears both texts and resets `turn_sequence`.
ERROR/RECOVERING retain latest text so context remains visible.

### Final-review bug fixed

Phase 15 final review found that production RECOVERING can carry a non-OK error while the reused legacy GUI maps RECOVERING to `PROCESSING`. `app_gui` only accepts a non-OK error when its legacy visual state is `ERROR`, so recovery snapshots could be rejected.

Fix in `voice_assistant_ui_gui_adapter`:

```text
legacy visual != ERROR -> translated last_error = ESP_OK
legacy visual == ERROR -> preserve non-OK production error
                          or ESP_FAIL fallback
```

The production UI model keeps the real error unchanged.
Fix commit: `d0d7a5b7b4fd1c52e163a917050420289ccd1684`.

### MVP visual limitation

Production backend knows exact:

```text
CONNECTING / READY / LISTENING / THINKING / SPEAKING / RECOVERING / ERROR
```

The reused Phase-12 GUI surface currently displays:

```text
CONNECTING -> PROCESSING
THINKING   -> PROCESSING
RECOVERING -> PROCESSING
READY      -> READY
LISTENING  -> LISTENING
SPEAKING   -> RESPONDING
ERROR      -> ERROR
```

This is an accepted Phase-15 MVP presentation limitation. Do not claim exact CONNECTING/THINKING/RECOVERING LCD labels.

## Production composition / validation isolation

The main composition still starts the real audio manager first, then production voice/UI/PTT/uplink/downlink/GPIO stack. It does not auto-begin a Xiaozhi session at boot.

When `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=y`, the production Phase-14/15 voice stack is suppressed so validation does not intentionally compete with production voice/UI ownership.

## Cross-system concurrency conclusion

Sensor, Firebase/cloud, GUI and normal FreeRTOS tasks may continue while Xiaozhi records/sends/plays audio. Correctness depends on owned resources and bounded callbacks/queues rather than globally pausing unrelated tasks.

```text
CPU/task interleaving                          allowed
Firebase + Xiaozhi network coexistence         allowed, HIL must measure contention
multiple clients directly accessing I2S        forbidden
multiple legitimate audio requests             require audio-manager arbitration
```

### Architecture follow-up — general audio arbitration

Before enabling multiple competing playback/capture clients, add/review centralized capture/playback arbitration in `audio_manager` (source/priority/interruptibility or equivalent). Examples include notification vs Xiaozhi playback, another recorder vs Xiaozhi listening, or critical alarm vs response playback.

This remains a post-Phase-14/15 architecture enhancement, not a reason to reopen the scoped PTT/UI MVP before first HIL.

## Known acceptance/build risks

1. Phase-14/15 build/link is not yet verified with target ESP-IDF toolchain.
2. Phase-14/15 target HIL has not run.
3. Actual Xiaozhi response audio codec must be proven; Opus evidence requires decoder work before Phase-14 speaker acceptance.
4. Phase-14 response playback is PSRAM/SD-backed rather than low-latency streaming.
5. GPIO5 PTT is temporary.
6. Firebase/cloud + Xiaozhi simultaneous load remains unmeasured.
7. General multi-client audio arbitration is not implemented.
8. Phase-15 source-scoped `esp_xiaozhi_chat_init` semantic bridge requires real build compatibility evidence against pinned `esp_xiaozhi` 0.1.2.
9. Phase-15 exact CONNECTING/THINKING/RECOVERING GUI labels are intentionally not implemented in the reused legacy visual enum.

## Next-work guidance

When asked **"hiện tại nên làm gì tiếp theo?"**:

- Always surface Phase-12 and Phase-13 Codex-ready HIL first.
- Mention Phase-14 HIL plan/test-branch pending status.
- Mention Phase-15 HIL plan/test-branch pending status.
- If hardware is unavailable, keep all HIL marked DEFERRED.
- Phase 14 software is complete.
- Phase 15 software is complete.
- Mention the general audio-arbitration follow-up before enabling competing audio clients.
- Do not start Sprint 16 automatically; wait for explicit user direction.

When hardware is available, recommended order is Phase 12 HIL -> Phase 13 HIL -> Phase 14 PTT voice HIL -> Phase 15 voice UI HIL -> full Gateway/Firebase integration regression using production branches only.

The full integration regression must include simultaneous Firebase/cloud and Xiaozhi traffic and any enabled competing audio-client scenarios.
