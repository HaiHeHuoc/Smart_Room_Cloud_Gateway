# Xiaozhi Phase 12 Hardware Acceptance Data

## Status

This document is a data-capture contract, not an acceptance claim. Phase 12 is
not complete. P2-E/P2-F target-hardware evidence and P2.6 staged repeated
lifecycle evidence remain required.

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

Do not archive raw `ESP_XIAOZHI_CHAT`, `esp_mcp_mgr`, or `esp_mcp_engine`
payload logs. During a valid P2.6 run those tags are temporarily suppressed;
the required evidence is the safe `XIAOZHI_FOUNDATION` segment and its final
summary.

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

Enable the master gate and `Run Phase 12.6 repeated lifecycle matrix`; P2-F
must remain unselected. Set the cycle count progressively to **1, 3, 10, 20,
then 100**, proceeding only after the preceding target run is clean. The
runner intentionally stops after the first failing cycle; do not hide a failed
stage by immediately increasing the count.

For each run capture the complete non-sensitive `XIAOZHI_FOUNDATION` segment,
including:

1. `XZ_LC_CASE duplicate_validation_request` with expected and actual
   `ESP_ERR_INVALID_STATE`.
2. For every completed cycle, matching `XZ_LC_BEGIN cycle=<n>
   generation=<n>` and `XZ_LC_END cycle=<n> result=PASS`, plus the P2.6
   WebSocket/P2-E cleanup facts and `RESOURCE[BEFORE_XIAOZHI]` /
   `RESOURCE[AFTER_CLEANUP]` samples.
3. The final `=== XIAOZHI LIFECYCLE SUMMARY ===`, containing requested,
   completed, passed, failed, first-failure/error, aggregate lifecycle
   counters, and aggregate resource minima.

The expected full cycle is WebSocket info/init/start/`CONNECTED`, one P2-E
audio open/close, then stop/deinit/destroy MCP. A later cycle must use fresh
context/EventGroup/handler/chat/MCP storage; a stale event, handle, or callback
pointer satisfying a later cycle is a failure. There must be no unexpected
worker overlap, MQTT fallback, secret exposure, crash, watchdog, task/stack
warning, or cleanup error.

Investigate before advancing if free-byte values show a persistent negative
post-cleanup trend, largest blocks decrease, or the aggregate worker
high-water mark shrinks unexpectedly. Internal and DMA-capable pools overlap
and must not be summed. The diagnostic does not establish CPU utilization,
socket count, TLS allocation size, or packet-loss rate. Wi-Fi-loss, service
fault, and timeout injection are deliberately separate future validation work;
P2.6 does not simulate them or take Wi-Fi reconnect ownership.

### Current P2.6 HIL Boundary

The target completed the progressive lifecycle summaries 1/1, 3/3, 10/10,
20/20, and 100/100 with expected duplicate-request rejection and no captured
panic/watchdog/assert or raw payload-tag output. The 100-cycle captured
post-cleanup boundaries nevertheless showed a material Internal free/largest
block decline. Treat this as a resource-stability **BLOCKED** result pending a
repeatable source-specific audit; it is not evidence sufficient to assert a
project memory leak, nor does it authorize continued stress or a phase-close
claim.
