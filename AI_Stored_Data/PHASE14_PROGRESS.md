# Phase 14 Push-To-Talk Voice MVP Progress

Updated: 2026-08-26
Branch: `phase/14-ptt-voice-mvp`
Current checkpoint: **14-F — FINAL Review / Production Composition / Docs**
Status: **SOFTWARE COMPLETE / BUILD PASS / GOLDEN-PATH HIL PASS / CLOSURE IN PROGRESS**

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
Dedicated PTT GPIO38 (pull-down, active-high)
-> voice_assistant_ptt authorization
-> long-lived voice_assistant / xiaozhi_foundation session
-> real READY evidence
-> audio_manager live PCM16 capture contract
-> bounded voice_uplink queue/task
-> aggregate 960 PCM16 samples and encode one 60-ms Opus packet
-> Xiaozhi Opus audio channel
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

Closure review found that 14-A..E logic existed but production `main.c` had not yet started it and the PTT GPIO had only been reserved. 14-F closes that gap without rewriting the large `main.c` startup flow.

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

Current board assignment:

```text
GPIO38 ---- push button ---- 3V3
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

GPIO38 is selected because the current board uses GPIO48 for its NeoPixel LED. Re-check the exact board schematic before hardware design stabilization.

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
    -> bounded copied mic queue + 60-ms Opus encoding + turn coordination

voice_assistant_downlink
    -> complete Opus-packet queue + PCM16 decoding/aggregation + audio-manager playback request

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
- release while the remote channel is opening is revalidated before I2S capture;
- a zero-packet turn closes its channel instead of blocking the next PTT;
- uplink queue is bounded/non-blocking from the audio callback;
- downlink callback is bounded/non-blocking;
- downlink queue loss taints response and prevents false successful playback;
- response inactivity timeout: 15 s;
- speaker-idle wait: 10 s;
- playback completion wait: 60 s;
- repeated turns are serialized until prior downlink/playback is finished;
- stale/non-current long-lived session items are rejected;
- no unbounded reconnect loop;
- a press in voice ERROR requests one bounded recovery and requires a fresh press after IDLE;
- SD failures stay under `sd_card_manager` ownership.

## HIL result and remaining boundaries

The corrected ESP32-S3 image was built and flashed on COM4. The operator then
confirmed audible response on GPIO38 after three complete PTT turns. Each turn
produced complete Opus uplink packets, decoded response PCM16, an `ESP_OK`
SD-backed WAV diagnostic, and `VOICE_DOWNLINK: response PLAYBACK_COMPLETE`.
Two release-before-READY attempts also completed bounded cancellation without
authorizing capture after release. No panic, assertion, watchdog reset, or I2S
ownership error occurred in the corrected run.

The following fault-injection cases were not run and remain explicitly
deferred: stalled response, network loss during a turn, SD unavailable during
response, and queue-pressure/corrupt-response injection. The LCD `Starting...`
route is a separate Phase-15 UI concern; it does not invalidate the accepted
voice transport/speaker path.

Other retained boundaries:

1. Downlink currently aggregates response then uses an SD-backed WAV handoff.
   This is intentionally higher latency than direct streaming playback.
2. Long-lived `session_generation` is not a unique per-PTT-turn ID; repeated
   turns are protected mainly by serialized turn boundaries.

## Build and HIL evidence — 2026-08-26

- ESP-IDF 6.0.1 production build after Opus/recovery fixes: **PASS** (`2093/2093`);
- app binary: `0x21b670` bytes, 47% of the app partition free;
- Phase-12 validator: OFF;
- stale Phase-13 HIL generated-config symbols removed during reconfigure;
- source-local Phase-14 composition and stream-tap objects compiled and linked;
- corrected callback build includes the static WebSocket staging item and
  codec-task stack budgets;
- known non-fatal warnings: missing `ESP_ROM_ELF_DIR` gdbinit generation and
  one existing unused LVGL image helper.

## HIL handoff

Use:

`AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`

Dedicated test branch:

`test/phase14-ptt-voice-e2e-hil`

Target evidence now proves mic -> Xiaozhi -> response -> speaker and three
repeated turns. Phase-14 golden-path HIL is therefore PASS; the fault cases
listed above remain SKIP/deferred.

## Closure statement

**Phase 14 = Software Complete / Build PASS / Golden-path HIL PASS / Repository
closure in progress.**

After the documentation and Git checkpoint are closed, the next planned
checkpoint is Phase 15 voice/UI HIL. Do not start Phase 15 implementation in
this Phase-14 closure change.
