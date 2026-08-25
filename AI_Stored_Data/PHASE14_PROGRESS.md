# Phase 14 Push-To-Talk Voice MVP Progress

Updated: 2026-08-25
Branch: `phase/14-ptt-voice-mvp`
Current checkpoint: **14-B — Microphone / Public Streaming Contract**
Status: **CONTRACT IMPLEMENTED / PRODUCER HOOK + BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 14 is implemented in reviewable parts. After each part, stop coding and wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

Planned checkpoints:

1. 14-A — PTT trigger + authorization state.
2. 14-B — microphone/public streaming contract.
3. 14-C — Xiaozhi audio uplink + connect audio-manager producer hook.
4. 14-D — response downlink + speaker playback.
5. 14-E — cancel/recovery/repeated-turn robustness.
6. 14-F — FINAL Phase-14 review/docs/deferred HIL.

When 14-F is reached, explicitly notify Hải that it is the final Phase-14 prompt before software closure.

## 14-A — completed

- Added `voice_assistant_ptt` policy task and press/release/cancel API.
- `capture_authorized=true` requires real `voice_assistant` READY evidence.
- Release-before-READY becomes `CANCEL_PENDING` and cannot authorize capture later.
- Temporary dedicated PTT pin reserved as GPIO5, active-high with internal pull-down. This assignment is explicitly temporary and will be changed by Hải later.

## 14-B — contract implemented

### Existing capture path reviewed

`audio_manager` already owns the complete INMP441 path:

```text
I2S RX
-> 256 stereo-slot frames/block
-> startup discard
-> microphone slot detection
-> selected mono PCM24 conversion
-> manager-owned PSRAM retained recording
-> DSP / playback
```

The safe Phase-14 publication point is after selected-slot conversion inside the audio-manager task. No I2S/DMA/private PSRAM pointer should cross the public boundary.

### Added public stream contract

New public header:

`components/audio/audio_manager/include/audio_manager_stream.h`

Contract baseline:

```text
sample rate : 16000 Hz
channels    : mono
format      : signed PCM16
granularity : up to 256 samples/frame
ownership   : borrowed callback frame only
```

`audio_manager_stream_frame_t` carries:

- borrowed `const int16_t *samples`;
- `sample_count`;
- sample rate / channel count;
- higher-level `stream_generation`;
- monotonic `frame_sequence` within the generation.

Consumers must copy/enqueue before callback return. They must never retain the pointer or perform blocking network/LVGL/I2S lifecycle work in the callback.

### Stream lifecycle API

Added:

```c
audio_manager_stream_register_callback(...);
audio_manager_stream_arm(generation);
audio_manager_stream_disarm(generation);
audio_manager_stream_get_status(...);
```

Arming/disarming controls **publication authorization only**. It does not start/stop I2S; capture lifecycle remains owned by:

```c
audio_manager_start_recording();
audio_manager_stop_recording();
```

The stream generation is non-zero and supplied by the higher-level voice transaction so late frames can be rejected downstream.

### Internal producer boundary

Added private helper:

`audio_manager_stream_publish_internal()`

in:

- `audio_manager_stream.c`
- `audio_manager_stream_internal.h`

It accepts a temporary manager-owned PCM16 block and invokes the borrowed callback only after leaving its short metadata critical section.

Stream metadata uses a static `portMUX_TYPE`; no lazy mutex allocation or extra lifecycle init is required.

### Important deliberate boundary

14-B establishes the public contract and safe publisher implementation, but **does not yet connect the existing PCM24 record loop to the publisher**. That source call-site is intentionally grouped with 14-C Xiaozhi uplink so the following can be reviewed as one complete data path:

```text
selected INMP441 PCM24 block
-> bounded PCM24 -> PCM16 conversion
-> audio_manager_stream_publish_internal()
-> voice transport queue / generation check
-> Opus/Xiaozhi uplink
```

Therefore current 14-B status is **contract implemented**, not live-microphone streaming PASS.

This is intentionally not implemented by exposing `s_runtime.recording_pcm24`, `s_rx_block`, I2S handles, or by creating a second I2S reader.

### Build graph

`audio_manager_stream.c` has been added to the `audio_manager` component CMake sources.

## Ownership preserved

- `audio_manager`: sole I2S/RX/DMA/PCM producer owner.
- `audio_manager_stream`: bounded project-owned publication metadata only.
- `voice_assistant_ptt`: authorization policy only.
- future 14-C transport consumer: must copy/enqueue and return quickly.

## Static review findings

1. No private I2S/DMA/PSRAM pointer is exposed publicly.
2. Stream callback executes outside the stream metadata critical section.
3. Generation is explicit and non-zero at arm time.
4. Duplicate arm is rejected; mismatched disarm is rejected.
5. No dynamic lock allocation remains in the stream registry.
6. The producer hook is deliberately not called yet, so no claim is made that live PCM frames are currently emitted.
7. No ESP-IDF build or target HIL PASS is claimed.

## Next checkpoint — only after user says `tiếp tục`

**14-C — Xiaozhi Audio Uplink + Live Producer Hook**

Planned scope:

1. connect selected microphone samples inside `audio_manager` to the PCM16 stream publisher;
2. arm/start capture only after PTT authorization;
3. add a bounded copied transport queue between audio callback and Xiaozhi work;
4. encode/send audio through the supported Xiaozhi public audio-channel API;
5. stop/disarm on PTT release/cancel/error;
6. use generation checks to reject stale frames;
7. keep callbacks non-blocking and `audio_manager` as sole I2S owner;
8. do not implement speaker/downlink playback until 14-D.
