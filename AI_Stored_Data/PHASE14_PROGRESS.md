# Phase 14 Push-To-Talk Voice MVP Progress

Updated: 2026-08-25
Branch: `phase/14-ptt-voice-mvp`
Current checkpoint: **14-E — Cancel / Recovery / Repeated-Turn Robustness**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 14 is implemented in reviewable parts. After each part, stop coding and wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

Planned checkpoints:

1. 14-A — PTT trigger + authorization state. ✅
2. 14-B — microphone/public streaming contract. ✅
3. 14-C — Xiaozhi audio uplink + live producer hook. ✅
4. 14-D — response downlink + speaker playback. ✅
5. 14-E — cancel/recovery/repeated-turn robustness. ✅
6. 14-F — FINAL Phase-14 review/docs/deferred HIL. NEXT

When 14-F is reached, explicitly notify Hải that it is the final Phase-14 prompt before software closure.

## 14-A summary

- `voice_assistant_ptt` owns PTT authorization policy only.
- `capture_authorized=true` requires a real READY production session.
- release-before-READY becomes `CANCEL_PENDING` and cannot later authorize capture.
- temporary GPIO5 reservation remains active-high with internal pull-down and must be replaceable later.

## 14-B summary

Added project-owned live microphone contract:

```text
16 kHz / mono / PCM16 / max 256 samples per borrowed frame
```

No I2S, DMA, or manager-owned PSRAM pointer crosses the boundary.

## 14-C summary

Implemented production microphone uplink:

```text
INMP441
-> audio_manager I2S RX
-> PCM24 selected microphone samples
-> bounded PCM16 stream tap
-> copied 8-frame queue
-> voice_uplink task
-> xiaozhi_foundation
-> Xiaozhi PCM audio channel
```

PTT release now stops MANUAL listening but intentionally leaves the audio channel open so the server can return response audio.

## 14-D summary

Implemented bounded response path:

```text
Xiaozhi callback
-> copied queue
-> 1 MiB PSRAM response buffer
-> TTS_STOP
-> close audio channel
-> canonical PCM16 WAV on SD under sd_card_manager lease
-> audio_manager_play_wav()
-> MAX98357 speaker
```

This keeps `audio_manager` as sole I2S TX owner. It is intentionally a Phase-14 MVP trade-off: response playback begins after aggregation rather than low-latency streaming.

## 14-E — implemented robustness

### 1. Missing TTS_STOP / stalled response is bounded

The downlink worker no longer blocks forever on `portMAX_DELAY` while collecting a response. It polls the queue every 100 ms and applies a 15-second inactivity timeout while `collecting=true`.

Timeout path:

```text
collecting
-> no response activity for 15 s
-> close audio channel best effort
-> responses_failed++
-> response_timeouts++
-> clear response buffer/turn state
-> reset downlink queue
```

This prevents one missing `TTS_STOP` or broken server response from permanently blocking later turns.

### 2. Queue loss taints the response

If the Xiaozhi callback cannot enqueue an audio/control item because the 8-item downlink queue is full, the current response is marked **tainted** using an atomic flag.

A tainted response is never packaged into WAV or played as if complete. `TTS_STOP` therefore fails deterministically with `ESP_ERR_INVALID_RESPONSE` instead of playing truncated/corrupted audio while claiming success.

### 3. Stale/session-invalid events are rejected

Before processing a downlink item, the worker verifies that the long-lived production session is still:

```text
active
READY
same non-zero session_generation
```

Items outside the current session are dropped and counted in `chunks_dropped_stale`.

Important limitation: Phase-13 `session_generation` identifies the **long-lived WebSocket session**, not each individual PTT turn. Because multiple PTT turns reuse one session, generation alone cannot distinguish a pathological late packet from turn N that arrives after turn N+1 has begun. 14-E therefore also serializes turn boundaries instead of overclaiming per-turn generation safety.

### 4. Repeated turns are serialized against downlink/playback

`voice_assistant_downlink_is_busy()` reports true while the prior turn is:

```text
collecting response
or
speaker playback requested/in progress
```

`voice_uplink` will not open a new microphone/Xiaozhi turn while downlink is busy. It also requires `audio_manager` to be `IDLE` before opening a new uplink.

This prevents:

```text
turn N speaker PLAYBACK
+ turn N+1 microphone RECORDING
```

from racing for audio ownership.

### 5. Playback completion is now real, not merely command acceptance

14-D originally counted `responses_completed` immediately after `audio_manager_play_wav()` accepted the command. 14-E changes this:

```text
play_wav accepted
-> playback_requested=true
-> wait until AUDIO_MANAGER_STATE_PLAYBACK is observed
-> wait until it returns to IDLE
-> require last_error == ESP_OK
-> only then responses_completed++
```

Playback completion is bounded to 60 seconds. This makes repeated-turn gating correspond to actual speaker ownership rather than queue acceptance.

### 6. Speaker busy / SD unavailable are bounded failures

Before playback, downlink waits at most 10 seconds for `audio_manager` IDLE. Failure to obtain an SD lease or write the response WAV fails the response instead of bypassing `sd_card_manager` ownership.

The turn state is cleared after finalize failure so later turns can recover; no automatic infinite retry loop is introduced.

### 7. Network/session loss

Xiaozhi protocol errors are converted to a response ERROR item. The downlink path closes the audio channel best effort, records the failure, clears collection state, and resets the queue. Existing Phase-13 `voice_assistant` recovery remains the owner of long-lived session recovery; downlink does not reconnect transport itself.

## 14-E diagnostics added

`voice_assistant_downlink_status_t` now includes:

- `chunks_dropped_stale`;
- `response_timeouts`;
- queue-full drops;
- responses completed/failed;
- collected/buffered bytes;
- current collecting/playback flags;
- last error.

## Static review notes

1. Xiaozhi callbacks remain non-blocking and copy/enqueue only.
2. Queue-drop response taint uses C11 atomic state; the worker never reads a callback-written plain bool unsafely.
3. `voice_uplink` checks downlink busy state and audio-manager IDLE before opening a new turn.
4. Response completion is counted only after actual playback returns to IDLE.
5. Missing TTS_STOP cannot keep `collecting=true` indefinitely.
6. SD failure, audio busy timeout, response overflow, callback queue loss, and protocol error all have bounded terminal paths.
7. No automatic reconnect/retry loop was introduced.
8. ESP-IDF build and target HIL remain unverified.
9. Server response PCM-vs-Opus format still requires hardware evidence; if the server callback provides Opus despite the negotiated PCM channel, 14-D/14-E playback must gain a decoder before acceptance.
10. The Phase-14 MVP still uses SD-backed aggregated playback; low-latency streaming TX is a later optimization, not silently claimed complete.

## Next checkpoint — only after user says `tiếp tục`

**14-F — FINAL Phase-14 Review / Composition / Docs / Deferred HIL**

Planned final closure scope:

1. scan production application composition and bind the Phase-14 coordinators in the correct startup order without auto-triggering PTT;
2. review temporary GPIO5 reservation and ensure factory-reset button ownership remains untouched;
3. review all Phase-14 ownership/concurrency/build surfaces together;
4. reconcile roadmap/document wording with the implemented MVP boundary;
5. create/update Phase-14 HIL test plan and expected logs suitable for a later `test/...` branch;
6. update `AI_Stored_Data/NEXT_WORK_AND_HIL_BACKLOG.md`;
7. if no new software blocker is found, mark **Phase 14 = Software Complete / Build + HIL Pending**.

**The next `tiếp tục` prompt is the final Phase-14 implementation/closure prompt.**
