# Phase 14 HIL Test Plan — Push-To-Talk Voice MVP

Updated: 2026-09-02
Production branch: `phase/14-ptt-voice-mvp`
Status: **ORIGINAL GOLDEN-PATH HIL PASS / TARGETED REGRESSION HIL PARTIAL / FAULT CASES NOT RUN**

Retest closure note: the first Opus firmware run exposed a 5-KiB uplink-task
stack failure, and the next run exposed a 2-KiB response-item stack overflow on
the 4-KiB WebSocket task. The codec-owning tasks now use bounded documented
stack budgets, and the downlink callback stages its item outside the WebSocket
stack. The corrected image completed three consecutive target PTT turns with
audible response confirmed by the operator.

## Goal

Validate the complete Phase-14 production path on ESP32-S3 hardware without weakening ownership or inferring PASS from build/static review.

```text
GPIO38 PTT
-> PTT authorization
-> Xiaozhi READY
-> INMP441 capture
-> PCM16 capture -> 60-ms Opus uplink
-> server response
-> bounded downlink
-> SD-backed WAV handoff
-> audio_manager playback
-> MAX98357 speaker
-> next turn allowed only after cleanup
```

## Temporary PTT wiring

Current Phase-14 board reservation:

```text
3V3 ---- push button ---- GPIO38
                         |
                    internal pull-down

released = LOW
pressed  = HIGH
```

GPIO38 is the selected PTT input for the current board. GPIO48 remains reserved for the onboard NeoPixel. HIL evidence must confirm the actual GPIO38 edge markers.

## Mandatory preflight

1. Confirm exact production/test branch and HEAD SHA.
2. Confirm `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=n` for production-voice testing.
3. Build the exact firmware with the target ESP-IDF environment.
4. Confirm ESP32-S3 serial port and flash from the built image.
5. Verify INMP441, MAX98357/speaker, SD card, Wi-Fi/Internet and temporary PTT button are connected.
6. Capture serial log from reset.
7. Never classify target behavior PASS from expected logs alone.

Production build baseline recorded on 2026-08-25:

- ESP-IDF 6.0.1: **PASS** (`2088/2088`);
- binary `0x1efd60`, 52% app-partition free;
- production validator OFF;
- branch `test/phase14-ptt-voice-e2e-hil` already exists and carries the standardized Codex HIL runbook.

Target execution on 2026-08-26 used GPIO38 and passed clean build, flash,
boot/composition, two release-before-READY cleanup attempts, and three
complete PTT turns. Each turn produced Opus uplink packets, a TTS response,
decoded PCM16, SD-backed WAV playback, and `PLAYBACK_COMPLETE`; the operator
confirmed audible response. The corrected run had no panic, assertion, WDT,
I2S ownership error, or stale-turn playback. Cases 5-8 (fault injection) were
not run and remain explicitly deferred. Detailed markers and the earlier
failure evidence are recorded in `AI_Stored_Data/PHASE14_HIL_TEST_BRANCH.md`.

## Target execution result (2026-08-26)

| Case | Result | Evidence |
| --- | --- | --- |
| T14_01 build/flash | PASS | ESP-IDF 6.0.1 build `2093/2093`; COM4 flash verified |
| T14_02 boot/composition | PASS | GPIO38, Opus encoder/decoder and both coordinator tasks READY |
| T14_03 first PTT uplink | PASS | Opus packet generation and non-zero packet counters |
| T14_04 response/speaker | PASS | Opus decode, WAV_DIAG `ESP_OK`, playback complete; audible response confirmed |
| T14_05 repeated turns | PASS | Three complete turns returned to `IDLE` without crash or ownership error |
| T14_06 release before READY | PASS | Two early-release attempts ended in bounded cancellation/cleanup |
| T14_07 stalled response | SKIP | Not injected during this run |
| T14_08 network loss | SKIP | Not injected during this run |
| T14_09 SD unavailable | SKIP | Not injected during this run |
| T14_10 queue pressure | SKIP | Not injected during this run |
| T14_11 resource trend | PASS (observed) | Three turns completed; no monotonic failure or crash observed |

Representative corrected-run markers:

```text
VOICE_DOWNLINK: WebSocket callback stack_hwm=744 staging=static
VOICE_UPLINK: first Opus packet generation=1 bytes=180 stack_hwm=14820
VOICE_DOWNLINK: first Opus packet decoded generation=1 opus_bytes=2100 pcm_bytes=1920
AUDIO_MANAGER: WAV_DIAG result=ESP_OK ... tx_q_ovf=0 tx_timeout=0
VOICE_DOWNLINK: response PLAYBACK_COMPLETE generation=1
```

## Expected boot/composition evidence

After network handoff and audio startup, expected Phase-14 markers include:

```text
VOICE_ASSISTANT: ... IDLE
VOICE_PTT: ... IDLE
VOICE_UPLINK: coordinator started
VOICE_DOWNLINK: coordinator started ...
VOICE_PTT_GPIO: initialized gpio=38 active_level=1 poll=10ms debounce=40ms
VOICE_PTT_GPIO: started gpio=38 active_level=1 pull=down initial=released
VOICE_ASSISTANT: IDLE -> CONNECTING
XZ_SESSION: WebSocket production session CONNECTED
VOICE_ASSISTANT: CONNECTING -> READY
PH14_COMPOSE: Phase-14 voice stack READY; boot Xiaozhi connection queued
```

Acceptance:

- no Phase-12 transport validator auto-run;
- no panic/assert/WDT/Guru Meditation;
- factory-reset GPIO9 remains independently functional;
- audio manager reaches normal IDLE before PTT voice stack is used.

## Case 1 — First PTT uplink

Action:

1. Wait for the boot session to report `READY`, then hold PTT. If it is still
   `CONNECTING`, hold PTT and wait for real READY/LISTENING/capture evidence.
2. Speak one short known phrase only after capture has started.
3. Release PTT after speaking.

Expected logical sequence:

```text
VOICE_PTT_GPIO: edge=PRESS
VOICE_PTT: ... -> ARMING_SESSION
VOICE_ASSISTANT: IDLE -> CONNECTING
XZ_SESSION: ... READY
VOICE_ASSISTANT: CONNECTING -> READY
VOICE_PTT: ... -> AUTHORIZED ... authorized=yes
VOICE_UPLINK: turn START generation=N
AUDIO_MANAGER: ... RECORDING
VOICE_OPUS: encoder READY rate=16000 channels=1 frame_ms=60 ...
XZ_SESSION: audio channel READY generation=N format=opus rate=16000 channels=1 frame_ms=60
...
VOICE_PTT_GPIO: edge=RELEASE
VOICE_PTT: ... -> RELEASED ... authorized=no
VOICE_UPLINK: turn STOP generation=N result=ESP_OK opus_packets=N opus_bytes=N pcm_samples=N channel_retained=yes
```

Required evidence:

- mic/I2S capture starts only after authorization;
- uplink frame/byte counters increase;
- no queue-full trend severe enough to destroy the utterance;
- release revokes capture and does not close the response channel prematurely.

## Case 2 — Server response + speaker

After Case 1 release, expect server response activity:

```text
VOICE_DOWNLINK: response START generation=N
... response audio chunks ...
VOICE_DOWNLINK: response PLAYBACK_REQUESTED generation=N ...
AUDIO_MANAGER: ... WAV PLAYBACK ...
VOICE_DOWNLINK: response PLAYBACK_COMPLETE generation=N
```

Physical acceptance:

- speaker produces an intelligible server response;
- playback finishes and audio manager returns IDLE;
- no simultaneous mic capture and speaker I2S ownership;
- `responses_completed` increments only after actual playback completion.

Critical codec acceptance:

The server response callback is treated as one complete raw Opus packet. The
downlink task must decode each packet to 16-kHz mono PCM16 before WAV creation;
compressed bytes must never be wrapped directly in a PCM WAV.

## Case 3 — Repeated turns

Run at least 3 complete PTT turns:

```text
PRESS -> uplink -> RELEASE -> response -> speaker -> IDLE
```

Acceptance:

- each next turn begins only after prior downlink/playback is no longer busy;
- no stale response from the previous turn is played as the new turn;
- no `ESP_ERR_INVALID_STATE` loop;
- no monotonic resource loss that clearly grows per completed turn;
- no I2S ownership conflict.

## Case 4 — Release before READY

1. Press PTT while a new session must connect.
2. Release quickly before READY.

Expected:

```text
ARMING_SESSION
-> CANCEL_PENDING
capture_authorized=false
-> bounded connection resolution
-> cleanup
-> IDLE
```

Acceptance: microphone transmission must never become authorized after the early release.

## Case 5 — Missing/stalled response

Inject or naturally reproduce a server/network condition where TTS response stalls after response collection starts.

Expected after 15 seconds of inactivity:

```text
VOICE_DOWNLINK: response ABORT generation=N timeout=yes error=ESP_ERR_TIMEOUT
```

Acceptance:

- audio channel closes best effort;
- response state clears;
- later PTT turn remains possible after recovery;
- no infinite `collecting=true` state.

## Case 6 — Network loss during turn

Externally remove AP/Internet/service during an active turn. Do not simulate with private Xiaozhi APIs.

Expected ownership:

- Xiaozhi/session reports transport failure;
- uplink/downlink stop using the failed session;
- Phase-13 voice-assistant recovery remains the session-recovery owner;
- no project-owned unbounded reconnect loop; the upstream retained WebSocket
  session is allowed to reconnect first;
- no stuck I2S capture/playback.

After restoring network, wait for the retained session to return to `READY`.
If it has entered `ERROR`, hold PTT once through `RECOVERING -> IDLE`; the PTT
policy starts a fresh session and may authorize that same still-held press only
after real READY. Releasing first must cancel and must never authorize capture.

## Case 7 — SD unavailable during response

Remove/unavailable SD before downlink finalization only when safe to do so for the board/test setup.

Expected:

- response WAV handoff fails cleanly;
- `sd_card_manager` owns/report recovery;
- `audio_manager` is not bypassed;
- downlink clears turn state;
- response is FAIL, not PASS/SKIP.

## Case 8 — Queue pressure / corrupt-response protection

If downlink callback queue-full is observed:

- `chunks_dropped_queue_full` increments;
- current response becomes tainted;
- truncated response must not be played as successful audio;
- finalization should fail with invalid-response semantics.

## Resource checkpoints

Capture before first turn and after each completed turn when system has returned to IDLE:

- internal free heap;
- largest internal block;
- PSRAM free/largest block;
- task stack high-water where available;
- audio RX/TX timeout/overflow counters;
- uplink queue-full/stale drops;
- downlink queue-full/stale drops/timeouts.

Do not require byte-identical free heap. Investigate monotonic degradation correlated with each equivalent completed turn.

## PASS / FAIL rule

Phase-14 HIL PASS requires all mandatory golden-path cases plus repeated-turn acceptance and no critical ownership/crash regression. Manual fault cases may be recorded independently, but missing mandatory audio E2E evidence is not PASS.

Use only:

- `PASS` — direct target evidence satisfies criteria;
- `FAIL` — evidence violates criteria;
- `SKIP` — test not performed; never equivalent to PASS.

## Dedicated test branch

Use the existing dedicated branch:

`test/phase14-ptt-voice-e2e-hil`

The test branch may add deterministic markers/supervisor logic, but production bugs must be fixed on `phase/14-ptt-voice-mvp` and then propagated into the test branch.
