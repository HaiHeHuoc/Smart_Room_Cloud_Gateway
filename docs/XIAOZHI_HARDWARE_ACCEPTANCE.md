# Xiaozhi Phase 12 Hardware Acceptance Data

## Status

This document is a data-capture contract, not an acceptance claim. Phase 12 is
not complete. P2-E and P2-F target-hardware evidence, then Phase 12.6 repeated
lifecycle/fault evidence, remain required.

All collection below uses the temporary master gate:

```text
Component config -> Xiaozhi Phase 12 validation
    Enable temporary Phase 12 Xiaozhi validation runtime
```

Keep the gate disabled for normal Gateway use. Do not record or publish SSIDs,
passwords, tokens, activation/QR material, endpoint URLs, private keys, raw
audio, or unbounded transcript content.

## Common Capture Header

For every run, record the non-sensitive build identity, target board, ESP-IDF
version, selected checkpoint, gate/fixture settings, Wi-Fi availability, and
the complete `XIAOZHI_FOUNDATION` serial segment. Record the final result even
when it is `FAIL` or `NOT RUN`; never convert missing evidence into a pass.

The worker emits these low-frequency resource checkpoints when the master gate
runs a validation:

```text
RESOURCE[BEFORE_XIAOZHI]
RESOURCE[AFTER_CHAT_INIT]        # only after chat_init succeeds
RESOURCE[AFTER_CONNECTED]        # only after CONNECTED is received
RESOURCE[AFTER_VALIDATION]
RESOURCE[AFTER_CLEANUP]
VALIDATION SUMMARY
VALIDATION COUNTERS
RESOURCE DELTA after_cleanup-before_xiaozhi
```

Each resource sample includes these byte fields:

| Pool | Free | Historical minimum free | Largest free block |
|---|---|---|---|
| Internal | `internal_free_bytes` | `internal_min_free_bytes` | `internal_largest_block_bytes` |
| DMA-capable | `dma_free_bytes` | `dma_min_free_bytes` | `dma_largest_block_bytes` |
| PSRAM | `psram_free_bytes` | `psram_min_free_bytes` | `psram_largest_block_bytes` |

It also includes `worker_stack_hwm_words`, the validation worker's minimum
remaining FreeRTOS stack since task creation, in words. Internal and
DMA-capable heap pools overlap on ESP32-S3, so their values must be reported
separately and **must never be added together**. The minimum-free fields are
boot-lifetime low-water marks, not interval-only lows.

`RESOURCE DELTA` is `AFTER_CLEANUP - BEFORE_XIAOZHI`. A negative free-byte
delta is an investigation signal, not leak proof; one run cannot prove or
disprove a memory leak. CPU utilization, socket count, TLS allocation size,
and packet-loss rate are **NOT DIRECTLY OBSERVABLE** from this bounded
one-shot diagnostic and must not be inferred from heap deltas.

## P2-E WebSocket Audio-Channel Lifecycle

Enable only the master validation gate. Capture:

1. `CONNECTED`, selected `WebSocket`, `Opening audio channel`, and
   `AUDIO_CHANNEL_OPENED`.
2. The one-second stable hold without `DISCONNECTED` or `CHAT_ERROR`.
3. `AUDIO_CHANNEL_CLOSED`, successful stop/deinit/destroy cleanup, all resource
   samples, counters, delta, and `P2-E RESULT: PASS`.

P2-E passes only with the complete target serial trace above. A host build or
an isolated individual log line is not P2-E hardware acceptance.

## P2-F Known-Audio WebSocket E2E

Use a lawful local `XZF1` raw-Opus fixture only. Enable the master gate,
fixture embedding, and P2-F selection. Do not substitute PCM, WAV, Ogg, WebM,
or fabricated audio data.

Collect both kinds of target evidence:

1. Serial: `start_listening: OK`, non-zero TX frame/byte counters, successful
   `stop_listening`, non-empty USER and ASSISTANT `CHAT_TEXT` evidence,
   conversation completion, non-zero audio RX callback/byte counters, channel
   close, final cleanup, resource samples/delta, and `P2-F RESULT: PASS`.
2. Audible: an observer confirms audible server TTS on the intended target
   output path, with run timestamp and non-sensitive test description.

The serial trace proves protocol-side evidence; audible proof is a separate
physical-output observation. Neither can replace the other for P2-F.

## Phase 12.6 Repeated Lifecycle And Fault Matrix

After P2-E/P2-F acceptance, collect a planned and actual cycle count for
WebSocket init/start/connected/open-close/stop/deinit, plus per-cycle or
periodic resource checkpoint/delta samples. Include:

- normal repeated lifecycle cycles and final cleanup result;
- Wi-Fi loss while idle and while an audio channel is open, followed by
  recovery through the existing Wi-Fi owner;
- service/protocol error and timeout handling without a stale event satisfying
  a later cycle;
- no unexpected worker overlap, no project-side MQTT fallback, and no secret
  exposure in logs;
- comparison of first/last resource samples, historical minima, largest blocks,
  worker stack high-water, and any observed regressions.

Any persistent negative free-byte trend, decreasing largest block, cleanup
failure, stale-event symptom, or task/stack warning is a failure to investigate
before Phase 12 closure. This document deliberately does not invent a CPU,
socket, TLS, or packet-loss metric unavailable from the current diagnostic.
