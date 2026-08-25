# Phase 14 Push-To-Talk Voice MVP Progress

Updated: 2026-08-25
Branch: `phase/14-ptt-voice-mvp`
Current checkpoint: **14-D — Response Downlink + Speaker Playback**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 14 is implemented in reviewable parts. After each part, stop coding and wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

Planned checkpoints:

1. 14-A — PTT trigger + authorization state. ✅
2. 14-B — microphone/public streaming contract. ✅
3. 14-C — Xiaozhi audio uplink + live producer hook. ✅
4. 14-D — response downlink + speaker playback. ✅
5. 14-E — cancel/recovery/repeated-turn robustness. NEXT
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

## 14-C summary and 14-D lifecycle correction

The live uplink is:

```text
INMP441 -> audio_manager I2S RX -> selected mono PCM24
-> PCM16 256-sample tap -> bounded voice_uplink queue
-> xiaozhi_foundation -> public Xiaozhi binary audio API
```

14-D corrected one important lifecycle assumption from 14-C: PTT release must **stop MANUAL listening but keep the Xiaozhi audio channel open**, because the same channel is needed for server response audio.

Correct turn boundary is now:

```text
PTT release
-> stop live microphone publication
-> cooperative audio_manager_stop_recording()
-> esp_xiaozhi_chat_send_stop_listening()
-> KEEP audio channel open
-> receive server response
-> TTS STOP / completion
-> close audio channel
```

The long-lived Phase-13 WebSocket/chat session remains READY throughout a normal turn.

## 14-D — implemented

### Production response callback boundary

`xiaozhi_foundation` now promotes project-owned response event kinds:

```text
TTS_START
AUDIO
TTS_STOP
ERROR
```

via:

```c
xiaozhi_foundation_response_register_callback(...)
```

The response callback receives only borrowed event/audio data plus copied generation/error fields. It never exposes the Xiaozhi chat handle or protocol-private objects.

The production Xiaozhi audio callback publishes `RESPONSE_AUDIO`; the protocol callback maps `CHAT_TTS_STATE START/STOP` and chat errors into the project-owned response boundary.

### Bounded downlink coordinator

Added:

- `include/voice_assistant_downlink.h`
- `voice_assistant_downlink.c`

Xiaozhi callbacks do not perform SD or speaker work. They only copy response audio into an 8-item zero-wait queue. Each queue item is bounded to 2048 bytes; larger callback payloads are split into bounded copied chunks. Queue pressure is explicit through `chunks_dropped_queue_full` rather than blocking the Xiaozhi callback.

The downlink worker owns a bounded 1 MiB PSRAM response buffer. On TTS_START it resets the current response generation. AUDIO chunks are accepted only for that generation. Overflow aborts the response and closes the audio channel.

### Speaker handoff strategy for Phase-14 MVP

14-D deliberately does **not** create a second I2S TX owner and does not call the I2S driver from `voice_assistant`.

The current MVP uses a buffered handoff:

```text
Xiaozhi response callback
-> bounded copied queue
-> voice_downlink task
-> bounded 1 MiB PSRAM PCM16 aggregation
-> TTS_STOP
-> close Xiaozhi audio channel
-> write canonical 16-kHz mono PCM16 WAV to
   /sdcard/xiaozhi_response.wav under sd_card_manager lease
-> wait boundedly for audio_manager to return IDLE
-> audio_manager_play_wav()
-> existing audio_manager MAX98357A/I2S TX path
```

This preserves the strongest project ownership rule: `audio_manager` remains the sole I2S TX / MAX98357 owner.

Trade-off: response playback begins only after response completion and requires mounted SD. This adds latency and SD dependency. It is accepted as the Phase-14 software MVP boundary because the existing `audio_manager` has no public live PCM playback source yet. A true streaming speaker source should be introduced only through the audio-manager task/command ownership model, not by bypassing it from a new task.

### WAV safety / SD ownership

The downlink worker writes a standard 44-byte PCM WAV header for:

```text
format      PCM
sample rate 16000 Hz
channels    1
bits/sample 16
```

It acquires `sd_card_manager` before fopen and releases only after fclose. Confirmed file-media failures are reported back to SD recovery. Playback then uses the already-owned `audio_manager_play_wav()` path.

### Generation / bounded behavior

`voice_assistant_downlink_status_t` tracks:

- collecting/running/playback requested;
- session generation;
- bytes received/buffered;
- chunks queued;
- queue-full drops;
- completed/failed response count;
- last error.

Response chunks for a generation other than the currently collecting turn are ignored.

### Current response format assumption

The Phase-14 channel requests explicit `pcm`, 16 kHz, mono. The downlink worker therefore treats server callback bytes as PCM16 for the MVP. Build cannot validate this protocol property; target HIL must verify that the pinned Xiaozhi server/component actually returns compatible PCM bytes when the channel is opened with this format. If target evidence shows the server still returns Opus, the correct fix is a decoder/boundary change, not forcing those bytes through PCM playback.

## Ownership review

- `audio_manager`: sole RX/TX I2S, DMA and MAX98357 owner.
- `audio_manager_stream/tap`: copied microphone PCM publication only.
- `voice_assistant_ptt`: authorization policy only.
- `voice_assistant_uplink`: copied mic queue + transport sender.
- `voice_assistant_downlink`: copied response queue, bounded PSRAM aggregation and SD handoff only; never owns I2S.
- `xiaozhi_foundation`: sole direct `esp_xiaozhi` dependency and Xiaozhi audio-channel owner.
- `sd_card_manager`: sole VFS recovery/lease owner.

## Static review findings / limitations

1. ESP-IDF build has not been run in this ChatGPT environment; compile/link success is not claimed.
2. Target HIL has not been run.
3. Response PCM compatibility is an explicit HIL item.
4. The current buffered-SD playback is intentionally not low-latency streaming playback.
5. `voice_assistant_uplink` and `voice_assistant_downlink` are component-ready but final production application composition/start ordering is closed in 14-F.
6. Manual PTT recording still performs existing post-record DSP/retention after stop; optimization into a dedicated streaming-only capture mode remains a hardening item.
7. Response timeout, missing TTS_STOP, queue overflow recovery, repeated turns and stale callbacks are hardened in 14-E.
8. GPIO5 remains a temporary board assignment and is not yet production-bound to the PTT API.

## Next checkpoint — only after user says `tiếp tục`

**14-E — Cancel / Recovery / Repeated-Turn Robustness**

Planned scope:

1. harden missing/late TTS_START/TTS_STOP and response timeout;
2. close retained audio channel on cancel/network/session error;
3. ensure repeated turns cannot reuse stale response data/queued packets;
4. define behavior when SD is unavailable or speaker playback is busy;
5. reconcile uplink/downlink errors with PTT/session recovery without reconnect loops;
6. audit queue pressure/resource cleanup over repeated turns;
7. prepare deterministic HIL markers for later Phase-14 test branch.
