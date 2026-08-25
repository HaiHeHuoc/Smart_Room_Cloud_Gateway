# Phase 14 Push-To-Talk Voice MVP Progress

Updated: 2026-08-25
Branch: `phase/14-ptt-voice-mvp`
Current checkpoint: **14-C — Xiaozhi Audio Uplink + Live Producer Hook**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 14 is implemented in reviewable parts. After each part, stop coding and wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

Planned checkpoints:

1. 14-A — PTT trigger + authorization state. ✅
2. 14-B — microphone/public streaming contract. ✅
3. 14-C — Xiaozhi audio uplink + live producer hook. ✅
4. 14-D — response downlink + speaker playback. NEXT
5. 14-E — cancel/recovery/repeated-turn robustness.
6. 14-F — FINAL Phase-14 review/docs/deferred HIL.

When 14-F is reached, explicitly notify Hải that it is the final Phase-14 prompt before software closure.

## 14-A summary

- `voice_assistant_ptt` owns PTT authorization policy only.
- `capture_authorized=true` requires a real READY production session.
- release-before-READY becomes `CANCEL_PENDING` and cannot later authorize capture.
- temporary GPIO5 reservation remains active-high with internal pull-down and must be replaceable later.

## 14-B summary

Added the project-owned live microphone contract:

```text
16 kHz / mono / PCM16 / max 256 samples per borrowed frame
```

The consumer must copy/enqueue before returning. No I2S, DMA, or manager-owned PSRAM pointer crosses the boundary.

## 14-C — implemented

### Production data path

The implemented uplink path is now:

```text
INMP441
  ↓ audio_manager owns I2S RX
raw stereo slot samples
  ↓ existing startup discard + slot detection
selected mono PCM24 conversion
  ↓ Phase-14 source-local tap
PCM16 / 256 samples / 16 ms
  ↓ audio_manager_stream borrowed callback
copy into bounded queue (8 frames)
  ↓ voice_uplink task
xiaozhi_foundation_audio_uplink_send_pcm16()
  ↓
esp_xiaozhi public binary audio API
```

`audio_manager` remains the only I2S/RX/DMA owner. Networking never runs in the audio-manager callback/task path beyond a non-blocking queue copy.

### Live producer hook

Added `audio_manager_stream_tap.c` and private tap hooks.

Because `audio_manager.c` is a mature large capture owner, Phase 14 connects the producer with a source-local compile definition that redirects only that source file's call to:

```c
audio_dsp_convert_raw_slot_to_pcm24(...)
```

to the wrapper:

```c
audio_manager_stream_convert_raw_slot_to_pcm24(...)
```

The wrapper calls the original DSP conversion first, preserving its return value, and only performs stream publication while a stream generation is armed.

The tap discards the existing microphone-slot-detection conversions before collecting selected recording samples. It then forms 256-sample PCM16 frames and publishes them through the 14-B contract. Disarm immediately disables publication and drops a partial frame.

This implementation intentionally avoids a second I2S reader or exposing `s_rx_block` / `recording_pcm24` publicly. The geometry coupling (20 slot-detect blocks, 256 frames/block, 2 slots) is a static implementation dependency that must be covered by build/HIL and should be refactored into a direct capture-loop hook later if `audio_manager` is structurally changed.

### Voice uplink coordinator

Added:

- `include/voice_assistant_uplink.h`
- `voice_assistant_uplink.c`

The coordinator polls copied PTT status and uses an 8-frame copied queue. When PTT becomes AUTHORIZED:

```text
open Xiaozhi audio channel
→ enter MANUAL listening
→ arm audio stream generation
→ request audio_manager_start_recording()
```

On release/cancel/error/generation mismatch:

```text
revoke turn_active
→ disarm stream publication
→ request cooperative audio_manager_stop_recording()
→ stop Xiaozhi listening
→ close Xiaozhi audio channel
→ drain stale queued frames
```

Every queued frame carries the production session generation. Stale frames are dropped before transport send.

### Xiaozhi production audio boundary

`xiaozhi_foundation` now exposes:

```c
xiaozhi_foundation_audio_uplink_start(generation);
xiaozhi_foundation_audio_uplink_send_pcm16(...);
xiaozhi_foundation_audio_uplink_stop(generation);
xiaozhi_foundation_audio_uplink_get_status(...);
```

The Phase-14 MVP opens the supported public Xiaozhi audio channel with explicit parameters:

```text
format         = pcm
sample_rate    = 16000
channels       = 1
frame_duration = 16 ms
listening mode = MANUAL
```

The long-lived Phase-13 WebSocket/chat session remains READY after a normal PTT uplink stop, enabling later turns to reuse the same production session.

### Diagnostics / backpressure

`voice_assistant_uplink_status_t` tracks:

- frames queued;
- frames sent;
- queue-full drops;
- stale-generation drops;
- active generation;
- last error.

`xiaozhi_foundation_audio_uplink_status_t` tracks channel/listening state and transport frames/samples/bytes sent.

Queue send from the audio callback is zero-wait. Slow network transmission therefore cannot block I2S capture; queue-full becomes explicit diagnostic evidence rather than hidden audio-task blocking.

## Important remaining limitations

1. ESP-IDF build has not been run in this ChatGPT environment; compile/link success is not claimed.
2. Target HIL has not been run.
3. Current production manual recording still retains the whole recording and runs its existing post-record DSP after stop. 14-C uses the live pre-DSP PCM stream for Xiaozhi; optimization/removal of unnecessary retained post-processing for a future dedicated streaming capture mode is a later hardening item, not silently claimed done.
4. Server response audio callback remains intentionally unconsumed. Speaker/downlink ownership belongs to 14-D.
5. PTT GPIO is reserved but not yet bound into production application composition.

## Ownership review

- `audio_manager`: sole microphone/I2S/DMA owner.
- `audio_manager_stream/tap`: copied PCM publication only.
- `voice_assistant_ptt`: authorization policy only.
- `voice_assistant_uplink`: bounded queue + turn coordination.
- `xiaozhi_foundation`: sole direct `esp_xiaozhi` dependency and audio-channel owner.
- no LVGL or speaker ownership is introduced in 14-C.

## Next checkpoint — only after user says `tiếp tục`

**14-D — Response Downlink + Speaker Playback**

Planned scope:

1. copy server audio out of the Xiaozhi callback into a bounded project-owned queue;
2. add a public streaming playback input to `audio_manager` without exposing I2S;
3. play response audio through MAX98357A while keeping `audio_manager` sole TX owner;
4. use generation/lifecycle checks to reject stale response packets;
5. introduce/verify `THINKING -> SPEAKING` evidence from protocol events where supported;
6. keep all Xiaozhi callbacks non-blocking;
7. leave repeated-turn/fault robustness for 14-E.
