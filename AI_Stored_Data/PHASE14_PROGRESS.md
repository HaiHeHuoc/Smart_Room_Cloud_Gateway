# Phase 14 Push-To-Talk Voice MVP Progress

Updated: 2026-08-25
Branch: `phase/14-ptt-voice-mvp`
Current checkpoint: **14-F — FINAL Review / Production Composition / Docs**
Status: **SOFTWARE COMPLETE / STATIC REVIEW COMPLETE / BUILD + HIL PENDING**

## Final checkpoint status

1. 14-A — PTT trigger + authorization state. ✅
2. 14-B — microphone/public streaming contract. ✅
3. 14-C — Xiaozhi audio uplink + live producer hook. ✅
4. 14-D — response downlink + speaker playback path. ✅
5. 14-E — cancel/recovery/repeated-turn robustness. ✅
6. 14-F — production composition + final review + HIL handoff. ✅

No additional Phase-14 software implementation prompt is required unless build/HIL exposes a defect.

## Final production flow

```text
Dedicated PTT GPIO5 (temporary, pull-down, active-high)
-> voice_assistant_ptt authorization
-> long-lived voice_assistant / xiaozhi_foundation session
-> real READY evidence
-> audio_manager live PCM16 capture contract
-> bounded voice_uplink queue/task
-> Xiaozhi PCM audio channel
-> stop MANUAL listening on release while response channel remains open
-> Xiaozhi TTS/audio response callback
-> bounded voice_downlink queue + 1 MiB PSRAM aggregation
-> SD-managed canonical PCM16 WAV handoff
-> audio_manager_play_wav()
-> MAX98357 speaker
-> wait for real PLAYBACK -> IDLE completion
-> allow next serialized PTT turn
```

## 14-F production composition

Closure review found that 14-A..E logic existed but production `main.c` had not yet started it and GPIO5 had only been reserved. 14-F closes that gap without rewriting the large `main.c` startup flow.

Added:

- `main/phase14_voice_composition.c`;
- `voice_assistant_ptt_gpio.c` / `voice_assistant_ptt_gpio.h`;
- source-scoped composition redirects in `main/CMakeLists.txt`.

Only `main.c` sees these compile-time redirects:

```text
audio_manager_register_status_callback
-> app_phase14_audio_manager_register_status_callback

audio_manager_start
-> app_phase14_audio_manager_start
```

The real `audio_manager` implementation and public semantics are not renamed or changed for other components.

The wrapper:

1. preserves the existing main/application audio GUI callback;
2. fans copied audio status into `voice_assistant_audio_adapter`;
3. calls the real `audio_manager_start()`;
4. only after audio startup succeeds, initializes/starts:
   - `voice_assistant`;
   - `voice_assistant_ptt`;
   - `voice_assistant_uplink`;
   - `voice_assistant_downlink`;
   - dedicated PTT GPIO adapter.

Production voice does not auto-begin a session at boot. A physical PTT press remains the user authorization trigger.

## Dedicated PTT input

Temporary board assignment remains:

```text
GPIO5 ---- push button ---- 3V3
internal pull-down
released = LOW
pressed  = HIGH
```

The new GPIO adapter owns only polling/debounce and forwards stable edges to:

```c
voice_assistant_ptt_press();
voice_assistant_ptt_release();
```

It does not reuse or alter the factory-reset button on GPIO9.

Current temporary timing:

- poll: 10 ms;
- debounce: 40 ms;
- PTT GPIO task stack: 3072 bytes;
- PTT GPIO task priority: 4.

GPIO5 remains explicitly temporary; Hải plans to replace it later. Re-check the final hardware pin map before hardware design stabilization.

## Production vs Phase-12 validation isolation

`phase14_voice_composition.c` suppresses production voice startup when:

```text
CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=y
```

Therefore a test build that intentionally enables the Phase-12 validator does not also start the Phase-14 production voice stack. Normal Phase-14 production remains expected to keep validation OFF.

## Final ownership review

```text
factory-reset button_manager
    -> GPIO9 reset semantics only

voice_assistant_ptt_gpio
    -> dedicated PTT GPIO sampling/debounce only

voice_assistant_ptt
    -> user authorization policy

audio_manager
    -> sole microphone/speaker/I2S/DMA owner

audio_manager_stream/tap
    -> copied live PCM publication only

voice_assistant_uplink
    -> bounded copied mic queue + turn coordination

voice_assistant_downlink
    -> copied Xiaozhi response queue/aggregation + audio-manager playback request

xiaozhi_foundation
    -> sole direct esp_xiaozhi/MCP/audio-channel dependency boundary

voice_assistant
    -> long-lived conversation/session/recovery orchestration

app_gui / ui_manager_lvgl
    -> sole LVGL/UI ownership
```

No Phase-14 callback directly owns LVGL or I2S.

## Robustness retained from 14-E

- release-before-READY cannot authorize microphone capture;
- uplink queue is bounded/non-blocking from the audio callback;
- downlink callback is bounded/non-blocking;
- downlink queue loss taints response and prevents false successful playback;
- response inactivity timeout: 15 s;
- speaker-idle wait: 10 s;
- playback completion wait: 60 s;
- repeated turns are serialized until prior downlink/playback is finished;
- stale/non-current long-lived session items are rejected;
- no unbounded reconnect loop;
- SD failures stay under `sd_card_manager` ownership.

## Important HIL risks / unclaimed points

1. No ESP-IDF build/link was executed from this ChatGPT environment.
2. No ESP32-S3 target HIL was executed.
3. The source-local CMake stream tap and main composition redirects require real toolchain verification.
4. The Phase-14 MVP negotiates PCM audio; actual server downlink payload must be proven to be compatible PCM16. If target evidence shows Opus, a decoder is required before speaker acceptance.
5. Downlink currently aggregates response then uses an SD-backed WAV handoff. This is intentionally higher latency than direct streaming playback.
6. GPIO5 is temporary.
7. Long-lived `session_generation` is not a unique per-PTT-turn ID; repeated turns are protected mainly by serialized turn boundaries.

## Deferred HIL plan

Use:

`AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`

Recommended future dedicated test branch:

`test/phase14-ptt-voice-e2e-hil`

Do not claim Phase-14 HIL PASS until target evidence proves mic -> Xiaozhi -> response -> speaker and repeated turns.

## Closure statement

**Phase 14 = Software Complete / Build + Hardware Acceptance Pending.**

Do not start Phase 15 automatically. The next software roadmap task may be considered only after Hải explicitly asks to continue.
