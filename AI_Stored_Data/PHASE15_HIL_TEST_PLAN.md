# Phase 15 Voice Assistant UI — HIL Acceptance Plan

Updated: 2026-09-02
Production branch: `phase/15-voice-assistant-ui`
Recommended test branch: `test/phase15-voice-ui-hil`
Status: **PLAN READY / BUILD PASS / TARGETED TRANSPORT HIL PARTIAL / FULL UI-TEXT HIL PENDING**

## Goal

Prove that the Phase-15 production presentation layer shows real Xiaozhi lifecycle and semantic conversation text without breaking the Phase-14 golden-path PTT/audio behavior or LVGL ownership.

Phase 12 and Phase 13 are closed baselines. Phase 14 golden-path PTT/audio
HIL passed on GPIO38; its fault-injection cases remain deferred. This plan
verifies the additional Phase-15 text/UI contract without conflating those
inherited audio boundaries with UI failures.

## Hardware / prerequisites

- ESP32-S3 Smart Room Cloud Gateway target.
- Existing LCD/ST7735 + LVGL path working.
- INMP441 and MAX98357 connected as required by Phase 14.
- Phase-14 PTT GPIO38 wiring:

```text
3V3 ---- push button ---- GPIO38
internal pull-down
released LOW / pressed HIGH
```

GPIO48 remains reserved for the NeoPixel.

- Internet/Wi-Fi available for Xiaozhi.
- SD card available for current Phase-14 response WAV path.

## Evidence discipline

Do not mark PASS from source inspection alone.

Required evidence levels:

```text
IMPLEMENTED       code exists
BUILD VERIFIED    idf.py build/link succeeds
HIL PASS          target behavior/log/GUI evidence satisfies the case
```

## Test 1 — build/link

Run a clean ESP-IDF build on the Phase-15 production/test branch. The merged
production checkpoint passed this gate on 2026-09-02: `idf.py build`, ESP-IDF
6.0.1, binary `0x21cdd0`, 47% app partition free.

Acceptance:

- no compile/link error from `voice_assistant_ui_model`;
- no compile/link error from `voice_assistant_ui_gui_adapter`;
- source-local `esp_xiaozhi_chat_init` bridge compiles against pinned `esp_xiaozhi` 0.1.2;
- no duplicate/undefined callback/init symbol;
- no warning promoted to error.

Recorded result for the merged production branch: **PASS**. A target serial
trace proves boot/reconnect/capture/response/playback and busy-response PTT
rejection, but the visible LCD/text cases below remain pending.

The Phase-15 source-scoped semantic init bridge is a controlled seam; the
merged production build has proven compilation, while target callback behavior
remains part of the HIL cases below.

## Test 2 — production boot / validation isolation

Boot normal production configuration with Phase-12 validation disabled.

Expected high-level evidence:

```text
audio manager READY
voice_assistant started
production voice UI model started with semantic text observer
production voice GUI adapter started
Phase-15 voice stack READY; boot Xiaozhi connection queued ...
```

Acceptance:

- no boot loop;
- startup must leave the boot route when the voice lifecycle requests the
  Voice/Xiaozhi screen; a persistent `Starting...` screen is recorded as a
  Phase-15 routing failure, not as a Phase-14 audio failure;
- no LVGL watchdog/freeze;
- no duplicate Xiaozhi owner;
- dashboard remains available before an active voice transaction.

Then separately build/run Phase-12 validation configuration if needed and verify the production Phase-15 voice stack is suppressed.

## Test 3 — voice screen routing

Starting from the normal dashboard, press PTT.

Acceptance:

- an active voice lifecycle requests the Voice/Xiaozhi screen through `app_gui`;
- no Xiaozhi/voice callback calls LVGL directly;
- screen update occurs without crash/freeze;
- IDLE does not continuously steal the dashboard route.
- when microphone capture has actually begun, the LCD must show `RECORDING`
  (or the documented `RECORD` form) and a duration that advances; `READY`
  alone is not evidence that PTT capture works.

Current MVP visual mapping is intentionally:

```text
CONNECTING  -> PROCESSING
THINKING    -> PROCESSING
RECOVERING  -> PROCESSING
READY       -> READY
LISTENING   -> LISTENING
SPEAKING    -> RESPONDING
ERROR       -> ERROR
```

Do not fail the MVP only because CONNECTING/THINKING/RECOVERING share the `PROCESSING` label. Do fail if the screen does not update at all or if the backend/GUI state is inconsistent with the documented mapping.

## Test 4 — USER transcript

Perform one real PTT utterance that Xiaozhi can transcribe.

Expected behavior:

```text
Xiaozhi CHAT_TEXT / USER
-> xiaozhi_foundation semantic bridge
-> production UI model
-> app_gui
-> LCD User text
```

Acceptance:

- LCD shows the user transcript received from the real semantic event;
- text is not synthesized from logs;
- no crash for a long utterance;
- text remains null-terminated and bounded;
- stale text from a prior session does not overwrite the new session.

Record serial evidence for the active session generation and a photo/screenshot of the LCD if available.

## Test 5 — ASSISTANT text independent from audio

Complete the same turn and receive an assistant semantic response.

Acceptance:

- LCD shows real ASSISTANT text;
- assistant text can appear before/during speaker playback;
- UI does not wait for audio playback completion to update text;
- text presentation does not start/stop I2S or speaker directly;
- Phase-14 audio response path remains independently owned by `voice_assistant_downlink`/`audio_manager`.

## Test 6 — repeated turns in one long-lived session

Perform at least three successful PTT turns without intentionally tearing down the Xiaozhi session.

Expected latest-turn policy:

```text
Turn N complete:
User: <N user>
Assistant: <N assistant>

Turn N+1 USER arrives:
User: <N+1 user>
Assistant: <cleared>

Turn N+1 ASSISTANT arrives:
User: <N+1 user>
Assistant: <N+1 assistant>
```

Acceptance:

- `turn_sequence` advances for each accepted USER semantic text;
- assistant text from the previous displayed turn is cleared when a new USER turn arrives;
- ASSISTANT update does not increment the turn counter;
- there is no unbounded history allocation;
- no stale visible pairing of a new USER line with the previous turn's ASSISTANT line.

## Test 7 — new session stale-text cleanup

End/recover the session and start a new one so `session_generation` changes.

Acceptance:

- old USER text clears;
- old ASSISTANT text clears;
- presentation `turn_sequence` resets to zero before new semantic USER text arrives;
- a late old-generation text event is rejected and cannot repopulate the screen.

## Test 8 — ERROR and RECOVERING presentation

Cause a controlled network/service loss during or after a turn and invoke the normal recovery path.

Acceptance:

- production model retains its real error value;
- legacy GUI snapshot is accepted by `app_gui` during RECOVERING/PROCESSING;
- current latest-turn text remains visible while error/recovery state is presented;
- ERROR visual state carries a non-OK error;
- no GUI snapshot rejection loop;
- recovery does not directly manipulate LVGL from the recovery/network task.

This specifically validates the Phase-15 final-review fix that normalizes legacy GUI `last_error` semantics for non-ERROR visual states.

## Test 9 — truncation / text robustness

Use or inject a semantic USER/ASSISTANT string that exceeds the 192-byte presentation capacity if practical on the test branch.

Acceptance:

- output is bounded to 191 bytes plus NUL;
- matching `*_truncated` flag becomes true;
- no buffer overflow, invalid read or UI crash;
- subsequent normal text still renders correctly.

A test-only semantic injection helper may be added on the dedicated test branch if real server text cannot reliably produce the boundary case.

## Test 10 — LVGL ownership / UI stress

During several voice turns, allow sensor, Wi-Fi/cloud and normal GUI status updates to continue.

Acceptance:

- no direct LVGL call appears from Xiaozhi/voice callbacks;
- UI task remains responsive;
- no recursive LVGL mutex failure;
- no watchdog reset;
- screen routing does not corrupt the sensor/dashboard widgets.

## Test 11 — resource trend

Record before/after repeated-turn data for at least:

- internal free heap / minimum free heap;
- PSRAM free space if exposed;
- largest free block if available;
- app_gui task high-water mark;
- voice/UI model task/callback diagnostics if exposed;
- Phase-14 uplink/downlink queue-drop counters.

Acceptance: no obvious monotonic leak or rapidly worsening task-stack margin over repeated turns.

## Known non-blocking MVP limitations

These do not fail Phase 15 unless the observed behavior differs from the documented contract:

1. CONNECTING/THINKING/RECOVERING share the legacy `PROCESSING` visual label.
2. UI keeps only the latest USER/ASSISTANT turn, not scrollable full conversation history.
3. No waveform/audio-level visualization.
4. No GUI PTT button; physical GPIO38 is the Phase-14 PTT input and GPIO48 is reserved for NeoPixel.
5. Audio response codec/SD-backed playback belong to Phase-14 acceptance and may block full voice E2E independently of text/UI correctness.

## Suggested Codex activation for future test branch

```text
RUN PHASE 15 HIL
```

When implementing that activation on `test/phase15-voice-ui-hil`, Codex should:

1. build first;
2. flash/monitor the connected target;
3. execute tests independently and preserve logs/evidence;
4. distinguish a Phase-15 GUI/text defect from an inherited Phase-14 audio/transport defect;
5. fix test-harness-only defects on the test branch;
6. report production defects for correction on the owning production branch and propagate the fix forward before retest.

## PASS definition

Phase 15 may be marked HIL PASS only when production semantic USER and ASSISTANT text are visibly presented on the target, repeated latest-turn behavior is correct, error/recovery presentation does not get rejected, UI ownership remains intact, and no build/runtime blocker attributable to Phase 15 remains.
