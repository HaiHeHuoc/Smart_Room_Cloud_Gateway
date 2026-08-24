# Xiaozhi Phase 12 Hardware Acceptance Data

## Status

This document contains both the capture contract and the non-sensitive target
evidence collected on 2026-08-25. P2-E, repeated lifecycle/resource stability,
all seven supported controlled fault/recovery cases, and feature-off behavior
are accepted. Phase 12 remains **not complete** because no lawful local P2-F
fixture exists and real Wi-Fi/AP, Internet, and service-loss HIL is still
pending.

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
runs a validation. The application composition layer now waits for network
ONLINE, audio IDLE with no active I2S, a post-startup cloud state, the Xiaozhi
validation screen, and an uninterrupted 5000 ms quiescence window before the
first steady-state sample:

```text
RESOURCE[BEFORE_XIAOZHI]
RESOURCE[AFTER_CHAT_INIT]        # only after chat_init succeeds
RESOURCE[AFTER_CONNECTED]        # only after CONNECTED is received
RESOURCE[AFTER_VALIDATION]
RESOURCE[AFTER_CLEANUP]
RESOURCE[CLEANUP_T+0_MS]
RESOURCE[CLEANUP_T+250_MS]
RESOURCE[CLEANUP_T+1000_MS]
RESOURCE[CLEANUP_T+3000_MS]
RESOURCE[CLEANUP_T+5000_MS]
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

### Current P2-E Evidence

The ESP32-S3 target produced the complete WebSocket-selected P2-E sequence:
`CONNECTED`, `AUDIO_CHANNEL_OPENED`, bounded hold,
`AUDIO_CHANNEL_CLOSED`, successful `chat_stop`, successful `chat_deinit`, MCP
destroy, stable cleanup, and `P2-E RESULT: PASS`. Firebase authentication also
succeeded in the same boot and no panic, assert, watchdog, or MQTT fallback
was captured. **P2-E hardware result: PASS.** This proves the public
audio-channel lifecycle, not audio TX/RX or audible playback.

## P2-F Known-Audio WebSocket E2E

Use a lawful local `XZF1` raw-Opus fixture only. Enable the master gate,
fixture embedding, and P2-F selection. Do not substitute PCM, WAV, Ogg, WebM,
or fabricated audio data.

Collect serial evidence for `start_listening: OK`, non-zero TX frame/byte
counters, successful `stop_listening`, non-empty USER and ASSISTANT
`CHAT_TEXT`, conversation completion, non-zero audio RX callback/byte
counters, channel close, final cleanup, resource samples, and
`P2-F RESULT: PASS`.

Audible server-TTS playback is classified **SOURCE / ARCHITECTURE ACCEPTED
LIMITATION** for Phase 12. The validation boundary receives borrowed raw Opus
bytes only; it deliberately does not decode them or take `audio_manager`/
speaker ownership. Requiring audible playback here would introduce the
production adapter/audio path reserved for later sprints. This does not waive
the P2-F protocol evidence above. No lawful `p2f_fixture.bin` is present, so
**P2-F remains NOT RUN / BLOCKED** and no fixture was fabricated.

## Phase 12.6 Repeated Lifecycle And Controlled Fault/Recovery Matrix

Enable the master gate and select exactly one P2.6 mode while P2-F remains
unselected. The repeated lifecycle mode uses the 1, 3, 10, 20, then 100
progression. The controlled fault mode defaults to `None`; select one boundary
first, inspect it, then select further individual cases or `ALL_SUPPORTED`
only after a clean target result. The runner stops after the first unexpected
result or failed recovery; do not hide a failure by increasing the matrix.

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

For an actual controlled fault case, capture the complete safe segment with:

1. `XZ_FAULT_BEGIN`, `XZ_FAULT_INJECT`, and `XZ_FAULT_EXPECTED` for the
   selected safe project-owned boundary and expected `ESP_ERR_INVALID_STATE`.
2. `XZ_FAULT_CLEANUP result=PASS`, then a separate-generation
   `XZ_FAULT_RECOVERY result=PASS` from a fresh normal P2-E lifecycle.
3. `XZ_FAULT_END result=PASS`, every `XZ_FAULT_RESOURCE` boundary, and the
   aggregate `=== XIAOZHI FAULT SUMMARY ===` plus `XZ_FAULT_RESULT result=PASS`.
4. Absence of panic, abort, assert, watchdog, backtrace, stale-callback, raw
   upstream payload-tag, cleanup-error, overlapping-worker, and MQTT-fallback
   evidence.

The expected full cycle is WebSocket info/init/start/`CONNECTED`, one P2-E
audio open/close, then stop/deinit/destroy MCP. A later cycle must use fresh
context/EventGroup/handler/chat/MCP storage; a stale event, handle, or callback
pointer satisfying a later cycle is a failure. There must be no unexpected
worker overlap, MQTT fallback, secret exposure, crash, watchdog, task/stack
warning, or cleanup error.

The fault selector does not alter private Xiaozhi state, force an upstream
allocation failure, invoke an undefined lifecycle sequence, send raw protocol
data, call LVGL, or take Wi-Fi/provisioning/NVS/cloud/audio-hardware ownership.
It returns from a project continuation only after a safe public boundary; the
shared cleanup retains the primary injected error while separately recording a
cleanup error. A fresh context/EventGroup/handler/chat/MCP storage set is
required for recovery.

Investigate before advancing if free-byte values show a persistent negative
post-cleanup trend, largest blocks decrease, or the aggregate worker
high-water mark shrinks unexpectedly. Internal and DMA-capable pools overlap
and must not be summed. The diagnostic does not establish CPU utilization,
socket count, TLS allocation size, or packet-loss rate. Real Wi-Fi/AP,
Internet/DNS/TLS/service loss, remote timeout, server goodbye, malformed
remote response, and allocation-pressure evidence remain external or
source-audited validation; P2.6 does not simulate them or take Wi-Fi reconnect
ownership.

### Root-Cause Investigation And Current P2.6 Evidence

The old `BEFORE_XIAOZHI` sample was captured immediately after Wi-Fi became
ONLINE, before deferred `audio_manager`, `cloud_manager`/Firebase TLS, and the
validation GUI had reached comparable long-lived states. The later cleanup
sample included those allocations. The earlier -21,420 byte Internal and
-32,768 byte largest-block comparison is therefore retained but classified
`CONTAMINATED_BASELINE`; it cannot attribute the whole delta to Xiaozhi.

The source-specific three-repeat matrix produced:

| Stage | Public lifecycle | Target result |
|---|---|---|
| A | `get_info/free_info` | First pass -128 Internal and -8192 largest block; later passes flat. At 125 s, Internal was +6272 versus the first baseline while largest stayed at 81920: one-time fragmentation plateau. |
| B | MCP create/destroy | First pass +36 Internal, then flat. |
| C | MCP + chat init/deinit, no start | No post-warm-up Internal or largest-block slope. |
| D | WebSocket connect/stop/deinit | Before the fix, retained TLS allocations repeated per handshake. After the fix, first pass +40 Internal and later passes 0; largest block remained 77824 in the focused run. |
| E | Normal P2-E | Three cycles returned to 170727 Internal and 81920 largest block with no slope. |

The focused heap trace before the fix found nine retained allocations per TLS
handshake, with requested sizes `99+3+2+3+12+3+16+3+23 = 164` bytes. Their
call stacks converged on ESP-IDF 6.0.1
`esp_crt_ca_cb_callback()`/`esp_crt_copy_asn1()`: cross-signed certificate
verification copied `subject_raw` and issuer OID/value buffers, while the
temporary certificate cleanup did not own/free those copied buffers. Waiting
125 seconds (longer than two configured 60-second TCP MSL intervals) did not
release them. Disabling cross-signed verification removed the WebSocket trend
but caused Firebase TLS connection failure, so it was rejected.

The project now applies the upstream-compatible reference-lifetime repair only
to the audited ESP-IDF 6.0.1 source. CMake verifies SHA-256
`e44d1e0a42a9d33cfc072ea005e93c8a0337c5ebcbfb9a1cf1554930f4f2816f`,
generates the patched translation unit in the build directory, replaces only
the original mbedTLS target source, and fails configuration on source drift.
Managed components and the installed SDK are not modified. Remove/re-audit the
shim when upgrading to an IDF release that contains the upstream fix.

Upstream references: [ESP-IDF issue #18550](https://github.com/espressif/esp-idf/issues/18550),
[cross-signed compatibility issue #18512](https://github.com/espressif/esp-idf/issues/18512),
and the [current upstream certificate-bundle source](https://github.com/espressif/esp-idf/blob/master/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c).

Root-cause classification is **G. MIXED_CAUSE**:

- baseline contamination from concurrent Gateway startup;
- one-time allocator fragmentation/warm-up that reaches a stable plateau;
- a proven upstream ESP-IDF 6.0.1 cross-signed certificate-bundle leak;
- no retained project-owned Xiaozhi object was found.

After the repair, the staged lifecycle progression passed 1/1, 3/3, 10/10,
20/20, and 100/100. The 100-cycle aggregate recorded 100 connected, 100
disconnected, 100 audio-opened, 200 audio-closed, zero chat errors, minimum
Internal 164263 bytes, minimum largest Internal block 81920 bytes, minimum
DMA 156475 bytes, minimum PSRAM 6223540 bytes, and worker HWM 2976 words.
The final t+5000 sample was 170691 Internal and 81920 largest block: +6392
Internal and zero largest-block change versus the first t+5000 sample. There
was no monotonic loss trend. **Resource stability: PASS.**

`ALL_SUPPORTED` then passed all seven controlled boundaries. It observed seven
expected injected errors, seven cleanups, seven fresh-generation P2-E
recoveries, zero unexpected results, and no panic/assert/watchdog/stale
callback/MQTT fallback. In the audio-open case, Internal recovered from 166339
at t+0 to 170719 at t+5000, directly demonstrating bounded deferred cleanup.
**Controlled fault/recovery: PASS.**

### External Fault Classification

| Case | Classification | Evidence boundary |
|---|---|---|
| Wi-Fi/AP loss | `STILL_PENDING` | Requires a real AP outage while preserving `wifi_manager` reconnect ownership. |
| Internet loss | `STILL_PENDING` | No controllable external network outage was available. |
| DNS failure | `SOURCE_AUDITED / ACCEPTED_LIMITATION` | Public start/error paths and bounded waits preserve the primary error; no private resolver injection is used. |
| TLS failure | `SOURCE_AUDITED / ACCEPTED_LIMITATION` | Public transport errors are propagated and cleanup is ordered; unsafe certificate/private transport mutation is prohibited. |
| Service loss | `STILL_PENDING` | Requires a real remote outage or server-controlled evidence. |
| Server goodbye | `SOURCE_AUDITED / ACCEPTED_LIMITATION` | Public goodbye event becomes a sticky runtime failure and wakes bounded waits; it is not fabricated. |
| Remote timeout | `SOURCE_AUDITED / ACCEPTED_LIMITATION` | Connection/audio/conversation waits are finite and clean up through the public lifecycle. |
| Malformed response | `SOURCE_AUDITED / ACCEPTED_LIMITATION` | `get_info`/chat public errors are retained and partial info is freed; raw responses are not injected. |
| Allocation pressure | `SOURCE_AUDITED / ACCEPTED_LIMITATION` | Project/upstream allocation returns are checked; forced global exhaustion is intentionally unsafe and not run. |

### Feature-Off Regression

After `idf.py fullclean`, `idf.py reconfigure`, and `idf.py build` with the
master gate and heap tracing off, the image built at `0x1c7ad0` bytes. The
target booted normal network/UI/cloud/audio services, entered audio ready, and
completed Firebase sign-in. Across a 120-second filtered observation there was
no Xiaozhi worker, validation screen route, lifecycle/fault/P2 marker, panic,
assert, or watchdog. **Feature-off target regression: PASS.**
