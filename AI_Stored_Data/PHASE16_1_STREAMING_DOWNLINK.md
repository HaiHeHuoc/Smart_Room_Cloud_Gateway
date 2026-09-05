# Phase 16.1 — Xiaozhi Streaming Downlink

## Status

```text
Implementation       COMPLETE
Host contracts       PASS
ESP-IDF build        PASS
Target HIL           AUTOMATED MATRIX PASS / AUDIBLE RECOVERY CONFIRMED
```

## Scope

Replace the previous full-response path:

```text
Opus decode -> 2 MiB PSRAM aggregation -> SD WAV -> playback
```

with:

```text
WebSocket callback (copy only)
-> bounded packet queue
-> downlink worker Opus decode
-> bounded manager-owned PCM16 PSRAM ring
-> audio_manager task (sole I2S/DMA owner)
-> MAX98357A
```

The ring accepts complete signed PCM16/16 kHz/mono packets only. It uses a
1.44-second prefill before TX, preserves the existing fixed PCM16 output scale
plus volume mapping, and starts a short response at EOS even if it does not
reach the prefill watermark. Its bounded PSRAM capacity is 7.68 seconds.

## Arbitration and failure contract

- Xiaozhi reserves the stream through the Phase-16 playback arbiter at TTS_START.
- The downlink worker never writes I2S or retains a Xiaozhi callback buffer.
- TTS_STOP marks EOS and returns to the worker loop; it does not block waiting
  for the full playback duration.
- A full PCM ingress ring applies a finite retry/backpressure window to the
  unchanged decoded packet; it fails only after that deadline, a tainted
  callback queue, decode/protocol error, or playback starvation. No PCM is
  silently dropped or partially copied.
- A post-start dry ingress feeds 16 ms PCM-silence blocks for up to eight
  seconds. This preserves I2S continuity through transient cloud jitter
  without replaying the preceding DMA block; a longer outage remains a
  bounded playback failure.
- Generic cancellation, priority preemption, normal EOS completion, and
  producer failure retain distinct terminal request states.
- A higher-priority alarm can preempt only an interruptible Xiaozhi stream;
  manager-owned cleanup completes before the next I2S owner starts.

## HIL acceptance

1. Normal PTT response: observe `response START ... mode=pcm16_stream`.
2. Confirm `PCM_STREAM START` and actual manager `PLAYBACK` before `response DRAINING`/TTS_STOP for a normal multi-frame answer.
3. Hear a continuous answer; then observe `response PLAYBACK_COMPLETE` with
   accepted and played sample counts equal.
4. Repeat at least five turns without power cycling; verify no stale audio is
   heard on the next turn.
5. Exercise alarm preemption if the existing Phase-16 trigger is available;
   confirm the Xiaozhi request terminal state is PREEMPTED rather than COMPLETED.
6. Capture a delayed-stream/backpressure case only if safely reproducible;
   verify `PCM ingress backpressure` then `PCM ingress resumed` for a normal
   recovery, or a bounded `FAILED` result with no hang and a working subsequent
   PTT turn.

## Evidence performed

- Host WAV parser regression: 32/32 PASS.
- Host WAV stream contract: 3/3 PASS.
- Host PCM stream-core contracts: 3/3 PASS (copy ownership/wrap, atomic
  backpressure and stale generation, EOS/abort generation isolation).
- ESP-IDF 6.0.1 build: PASS; app binary `0x2203b0`, 47% partition free.
- Target automated HIL matrix T16.1-01 through T16.1-05: PASS. A live
  recoverable downlink gap emitted bounded silence then resumed, and operator
  audible continuity was confirmed after the recovery hardening.

Long-duration cloud/Firebase coexistence and endurance remain separate
integration acceptance work; this document does not claim them as complete.
