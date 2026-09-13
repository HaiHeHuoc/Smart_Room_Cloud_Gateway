# ESP32-S3 Smart Room Cloud Gateway — Xiaozhi Implementation Roadmap

**Status:** Active roadmap / implemented through Sprint 17; Sprint 18 in progress; Sprints 19-24 planned  
**Target:** ESP32-S3 N16R8, ESP-IDF 6.0.1  
**Resolved dependency:** `espressif/esp_xiaozhi: 0.1.2` (manifest constraint: `^0.1.1`)
**Voice MVP closure:** End of Sprint 15  
**Advanced voice closure:** End of Sprint 24

> This roadmap extends the existing project roadmap. It does not replace,
> reorder, skip, or silently close Sprints 0-9. Pending acceptance work in the
> original roadmap remains visible and higher priority unless explicitly
> deferred.
>
> **Numbering reconciliation (2026-09-13):** completed implementation history is
> preserved. Sprint 16 is the accepted Audio Arbitration & Multi-Client Audio
> Policy work, with Phase 16.1 as the Xiaozhi PCM streaming-downlink extension.
> Sprint 17 remains MCP read-only and Sprint 18 remains MCP controlled actions
> with all current Phase 18.x numbering/scope unchanged. The approved future
> roadmap is Sprint 19-23 Local Web Control V1-V5, followed by Sprint 24 Wake
> Word + Advanced Voice UX. The former Sprint 19 wake-word plan is deferred to
> Sprint 24 without changing its required content/order. Do not reuse or
> renumber Sprint 0-18 history.

---

## 1. Required Order

```text
Sprints 0-9 and pending acceptance
    -> Sprint 10 audio hardware validation
    -> Sprint 11 production audio_manager
    -> Sprint 12 esp_xiaozhi foundation
    -> Sprint 13 project-owned voice_assistant adapter
    -> Sprint 14 push-to-talk and limited multi-turn MVP
    -> Sprint 15 GUI voice and interactive chatbot animation
    -> Sprint 16 audio arbitration and multi-client audio policy
    -> Phase 16.1 Xiaozhi PCM streaming downlink
    -> Sprint 17 MCP read-only tools
    -> Sprint 18 MCP controlled actions
    -> Sprint 19 Local Web Control V1: SD Card File Manager
    -> Sprint 20 Local Web Control V2: Playback + Volume
    -> Sprint 21 Local Web Control V3: Lights
    -> Sprint 22 Local Web Control V4: Dashboard + System Status
    -> Sprint 23 Local Web Control V5: Scenes + Logs + Diagnostics
    -> Sprint 24 wake word and advanced voice UX
```

Sprints 19-23 are local-web frontend phases over existing project-owned
managers/services. They do not configure/control Wi-Fi and do not transfer
domain ownership away from existing managers. Detailed scope is in
`AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.

## 2. Ownership

```text
wifi_manager             owns Wi-Fi Station lifecycle and reconnect
provisioning_manager     owns temporary BLE provisioning transport
config_manager           owns persistent application configuration
cloud_manager            owns Firebase telemetry and retry policy
audio_manager            owns microphone, speaker, I2S, DMA staging and PCM flow
xiaozhi_foundation       owns the direct managed esp_xiaozhi/provider boundary
voice_assistant          owns product voice-session orchestration and protocol adaptation
app_gui                  owns GUI models, queues, screens and animation selection
ui_manager_lvgl          owns LVGL runtime and synchronization
lvgl_image_handler       owns decoded image/GIF resources and active image objects
device/application APIs  own validated sensor and actuator operations
```

Only `xiaozhi_foundation` may directly depend on managed `esp_xiaozhi` provider
handles/types. Provider handles, enums, callback-lifetime payloads, credentials,
and transport objects must not leak into unrelated public component APIs.
`voice_assistant` consumes only the project-owned foundation boundary.

No Xiaozhi, audio, network, MCP, Web, or LCD callback may directly take over
LVGL, Wi-Fi lifecycle, provisioning, application NVS reset, reboot, OTA, or
arbitrary hardware-driver ownership. Web and LCD remain frontends over
project-owned managers/services.

---

## 3. PSRAM-First Policy

Use PSRAM for bulk and long-lived data, while reserving internal RAM for DMA,
ISR, cache-off, synchronization, and hardware-critical work.

Every new allocation must be classified:

```text
INTERNAL_REQUIRED
DMA_REQUIRED
PSRAM_PREFERRED
PSRAM_REQUIRED
```

Rules:

1. Bulk allocations must not silently fall back to internal RAM.
2. Verify PSRAM-required pointers with `esp_ptr_external_ram()`.
3. Keep I2S DMA buffers and descriptors in internal DMA-capable RAM.
4. Move task stacks individually after cache-off/NVS/flash audit; do not enable
   a blind global PSRAM-stack policy.
5. NVS, flash erase/write, factory reset, and OTA run on internal-stack tasks.
6. No per-frame `malloc/free` in the audio hot path; use bounded preallocated
   pools and rings.
7. Record internal, DMA, PSRAM, largest block, minimum free memory, CPU, and
   task high-water marks at every phase closure.

PSRAM candidates include PCM rings, OPUS queues, playback jitter buffers,
transcripts, emotion data, bounded JSON/MCP buffers, GUI voice queues,
selected worker stacks, and supported wake-word/AFE models.

Internal/DMA candidates include I2S DMA memory, ISR state, locks, small session
state, generation counters, library-owned network/TLS internals, and
storage/reset/OTA stacks.

## 4. Resource Readiness Gate

Initial engineering targets before integrated Push-to-Talk acceptance:

| Metric | Target |
|---|---:|
| Internal steady-state free | >= 24 KiB |
| Largest internal block | >= 12-16 KiB |
| Internal low-water during voice | >= 10-12 KiB |
| DMA free before audio start | >= 20 KiB |
| DMA low-water during voice | >= 6-8 KiB |
| PSRAM free | >= 6 MiB |
| Voice/audio task stack remaining | >= 20% |
| Heap-loss trend after 100 turns | None |
| Nominal overflow/underrun | None |
| Watchdog/Guru Meditation/LVGL assertion | None |

These are project gates and may be revised only from target-hardware evidence.

---

# Sprint 10 — Audio Hardware Validation

**Goal:** Prove microphone capture and speaker playback independently.

## Phase 10.1 — Hardware And Pin Audit

- [ ] Select a production digital I2S microphone.
- [ ] Confirm MAX98357A or selected I2S output path.
- [ ] Confirm speaker impedance, power, voltage, enable, and ground.
- [ ] Audit BCLK, WS/LRCLK, DIN, DOUT and optional MCLK against all existing
      GPIO ownership.
- [ ] Freeze the physically verified audio pin map.

## Phase 10.2 — Isolated Capture

- [ ] Build an isolated capture test.
- [ ] Start with 16 kHz, mono, signed 16-bit PCM.
- [ ] Record speech and silence for at least 30 seconds.
- [ ] Measure noise, DC offset, clipping, dropped samples, and overflow.
- [ ] Store long buffers in PSRAM; retain DMA staging internally.

## Phase 10.3 — Isolated Playback

- [ ] Play a known tone or PCM/WAV sample.
- [ ] Validate I2S TX lifecycle, volume, noise, distortion, and underrun.
- [ ] Store playback rings in PSRAM; retain DMA staging internally.

## Phase 10.4 — Coexistence Baseline

- [ ] Run audio with LVGL, SD, Wi-Fi, sensors, button, and cloud active.
- [ ] Measure CPU, internal/DMA/PSRAM, largest blocks, stacks, overflow, and
      underrun.
- [ ] Repeat at least 20 reboot/bring-up cycles.

## Acceptance

- [ ] Capture and playback each run for 30 minutes without uncontrolled error.
- [ ] No watchdog, crash, or LVGL failure.
- [ ] Pin map, format, DMA settings, and resource baseline are documented.

---

# Sprint 11 — Production `audio_manager`

**Goal:** Create the project-owned production audio abstraction.

## Proposed Structure

```text
components/audio/audio_manager/
├── CMakeLists.txt
├── Kconfig
├── audio_manager.c
├── include/audio_manager.h
└── docs/README.md
```

## Phases

### 11.1 Lifecycle And API

- [ ] Define idempotent init/start/stop/deinit.
- [ ] Define capture start/stop, bounded playback submission, and copied status.
- [ ] Do not expose I2S handles, DMA descriptors, or Xiaozhi types.

### 11.2 Buffer And Task Design

- [ ] Add bounded PSRAM capture/playback rings.
- [ ] Keep DMA memory internal.
- [ ] Use preallocated pools and finite timeouts.
- [ ] Define ownership, overflow/drop policy, underrun recovery, and shutdown.
- [ ] Measure task priorities, core affinity, and stack placement.

### 11.3 Diagnostics — Complete For The Direct-DMA Baseline

Implemented in the current direct-DMA stability path:

- [x] Copied lifecycle state, latest error, cycle counters, and explicit
      RX/TX I2S-active flags.
- [x] RX/TX requested and returned byte totals, RX/TX queue-overflow callbacks,
      timeout/partial-write counts, maximum blocking duration, and task stack
      high-water mark.
- [x] Thread-safe diagnostic snapshots across ISR, audio-task, and public
      status-reader contexts.

- [x] Explicitly classify runtime PCM queue occupancy and a true hardware
      playback-underrun event as unavailable in the current direct-DMA design;
      do not publish invented zero values or infer them from unrelated events.

The ESP-IDF 6.0.1 standard I2S API exposes queue-overflow callbacks but no
safe public runtime fill level or hardware underrun event. The current
direct-DMA stability flow has no manager-owned PCM ring, so it must not infer
either metric from a timeout or queue-overflow callback. This documented API
limit does not reopen or block Phase 11.3 and does not by itself justify adding
a PCM ring. If a later measured architecture introduces an application-owned
ring, its occupancy and starvation policy can add those metrics then.

### 11.4 SD/WAV Streaming Playback

**Goal:** Prove that a local audio-file source can stream bounded PCM into the
production playback path without coupling filesystem ownership into
`audio_manager`.

#### Phase 11.4.1 — SD/WAV Streaming Foundation — Implemented / Build Verified

- [x] Private `audio_wav` reader uses the already-mounted `sd_card_manager`
      VFS path and never owns SD SPI, FATFS mount/unmount, or LVGL filesystem
      lifecycle.
- [x] Bounded RIFF chunk parser validates canonical PCM16 mono 16-kHz WAV,
      skips unknown aligned chunks, validates data bounds, and rejects corrupt
      or unsupported input without assuming a 44-byte header.
- [x] One reusable 4 KiB PCM buffer is allocated once per private stream; no
      whole WAV is loaded and no allocation occurs per read chunk.
- [x] `audio_manager` owns a private playback-source slot and central source
      cleanup; the same single owner now selects either production WAV or the
      default-off golden recorded-PCM regression path.
- [x] Phase 11.4.2 connects this reader directly to the existing manager-owned
      TX path with bounded reads; no ring/task split is required without
      runtime evidence.

#### Phase 11.4.2 — Direct Bounded WAV Playback — Implemented / Build And Parser Host-Test Verified / Hardware Pending

- [x] Native fixtures exercise the private parser without SD hardware,
      including chunk ordering/padding, missing chunks, unsupported formats,
      and truncated/bounds failures.
- [x] Missing filesystem paths remain `ESP_ERR_NOT_FOUND`; existing malformed
      WAV files missing `fmt ` or `data` return `ESP_ERR_INVALID_RESPONSE`.
- [x] The single audio-manager task reads one reusable 4 KiB PCM16 chunk,
      explicitly decodes little-endian mono samples, duplicates them into the
      existing stereo TX staging block, and calls the proven
      `write_tx_frames()` implementation.
- [x] WAV output bypasses microphone DSP/PCM24 conditioning and applies only
      the configured linear playback-volume percentage.
- [x] Existing TX silence preload, pre/post silence, I2S lifecycle,
      diagnostics, `PLAYBACK` state/callback, source cleanup, and MAX98357A
      safe-LOW policy are reused.
- [x] Aggregate logs expose WAV bytes read/streamed, successful bounded-reader
      latency, elapsed time, failures, and per-operation TX deltas without
      per-read INFO spam. Phase 11.4.3 excludes failed reads from the maximum.
- [x] A default-off Kconfig regression submits one configured `/sdcard/...`
      WAV through the Phase 11.4.3 production request path.
- [ ] Validate 5/30/60-second WAVs, invalid/removal cases, SD latency under
      Gateway load, clean EOF, sound quality, and golden-path regression on
      target hardware.

The 32-case host suite validates the private RIFF/WAV parser, bounded reader,
and resume-seek bounds only. The ESP-IDF firmware build validates
compilation/linking. Neither
is evidence for real SD latency, I2S TX, MAX98357A output, audible continuity,
or hardware cancellation.

#### Phase 11.4.3 — Production Playback Control And Manager Lifecycle — Implemented / Build Verified / Hardware Pending

- [x] Normal `audio_manager_start()` creates the single manager task, waits for
      `IDLE`, and no longer starts the infinite golden soak by default.
- [x] `audio_manager_play_wav()` validates and copies one bounded path into a
      two-entry command queue. Conflicting operations are rejected rather than
      queued as a playlist; the component retains source ownership while its
      manager task remains the sole I2S owner.
- [x] `audio_manager_stop_playback()` sets a protected cancellation request.
      WAV streaming polls it before reads and between PCM and silence TX blocks,
      then performs manager-owned cleanup and reports controlled cancel as
      `IDLE` with `ESP_OK` rather than a false error.
- [x] `audio_manager_stop()` requests cancel/shutdown, waits a finite five
      seconds, never force-deletes the owning task, and returns successful stop
      to `INITIALIZED`. A stopped manager can start again and deinit releases
      the queue, event group, mutex, PSRAM, source, and audio resources.
- [x] WAV started/completed/failed/cancelled counters supplement the unchanged
      public GUI state enum; no LVGL dependency or direct GUI call was added.
- [x] Golden `run_cycle()` remains available through a default-off Kconfig
      stability mode and preserves the accepted I2S/DMA/DSP/capture constants.
- [x] The original direct 4 KiB streaming proof remains the bounded raw-reader
      layer. Phase 11.4.4 adds a private reader/prefetch worker and two bounded
      PSRAM PCM blocks so the manager does not wait for SD/FATFS while feeding
      TX. Linear WAV volume (`100%` equals source PCM16), existing TX staging,
      and first-error cleanup remain unchanged; there is no whole-file or
      per-chunk allocation and no second I2S owner.
- [x] ESP-IDF 6.0.1 `reconfigure` and firmware build pass; the parser/resume-seek
      fixture suite passes 32/32.
- [ ] Validate production play/cancel/stop/restart/deinit, 5/30/60-second WAVs,
      failure recovery, latency, sound quality, and golden MIC regression on
      target hardware in Phase 11.4.4.

#### Phase 11.4.4 — Bounded SD Prefetch Continuity — Implemented / Build And Parser Host-Test Verified / Hardware Pending

- [x] A private reader worker is the sole owner of the WAV `FILE *`, 4 KiB
      raw-reader buffer, and SD VFS lease. The audio-manager task remains the
      sole I2S RX/TX and DMA-staging owner.
- [x] Two bounded PSRAM slots hold 10 seconds each by default (320,000 bytes
      per slot for PCM16 mono 16 kHz). The reader fills the inactive slot in
      at-most-4-KiB raw reads while the manager plays a READY slot.
- [x] TX starts only after the first READY slot. At a later cache boundary, a
      missing READY slot is counted as software prefetch starvation and fails
      the WAV cleanly rather than waiting indefinitely with an empty source.
- [x] Cancellation and cleanup request reader stop, join it, then free the
      slots; the SD lease is released before recovery may unmount the VFS.
- [x] `WAV_DIAG` records raw-read/fill timing, initial/boundary wait, fill
      failures, starvation count, bounded SD resume attempt/success/wait, and
      reader stack high-water mark.
- [x] On one confirmed media read failure, close the sticky failed `FILE *` and
      release its VFS lease before a bounded wait for SD remount; reopen, verify
      unchanged metadata, seek to the last committed PCM offset, and resume at
      most once. Cleanup status is separate from the retained reader result.
- [ ] Validate continuity at 5/10/30/60-second boundaries, cancellation during
      initial fill/refill, SD error during refill, PSRAM margin, and full
      Gateway load on target hardware.

Reference flow:

```text
SD card
    -> sd_card_manager / mounted filesystem
    -> private WAV reader/prefetch worker with SD VFS lease
    -> two bounded PSRAM READY PCM16 blocks
    -> audio-manager task (sole I2S owner)
    -> PCM16 mono decode / stereo staging
    -> existing write_tx_frames() / I2S TX DMA
    -> MAX98357A
    -> speaker
```

- [x] Keep I2S and source ownership in `audio_manager`; it does not own
      SD-card mount/unmount or general filesystem lifecycle.
- [x] Open a local WAV file through the project-owned SD/storage path.
- [x] Parse RIFF/WAVE structure and locate `fmt ` and `data` chunks without
      assuming fixed chunk ordering.
- [x] Support canonical uncompressed PCM16 mono 16-kHz WAV; validate sample
      rate, channel count, sample width, block alignment, and data length before
      playback.
- [x] Reject stereo, other rates/widths, float, and compressed formats
      deterministically; do not resample or add formats in this phase.
- [x] Read and submit bounded chunks instead of loading the entire audio file
      into RAM or PSRAM.
- [x] Use a bounded producer/consumer prefetch handoff after SD latency made
      direct read/decode/TX submission a continuity risk. The worker performs
      bounded raw reads; the manager consumes only READY PSRAM blocks and
      remains the sole I2S owner.
- [x] Handle normal EOF, missing/unsupported/corrupt/truncated input, short
      reads, SD-unavailable state, TX errors, close errors, and defensive
      cleanup in the bounded proof path.
- [x] Add user stop/cancel and production source arbitration in Phase 11.4.3.
- [ ] Measure SD-read latency, TX errors, CPU, memory, and task stack while the
      rest of the Gateway is active on target hardware.
- [ ] Keep compressed codecs such as MP3/AAC/FLAC outside this baseline phase
      unless a later requirement explicitly justifies decoder integration.

### 11.5 Stress Closure

- [ ] Run 1,000 start/stop cycles.
- [ ] Test repeated init/deinit and partial-init cleanup.
- [ ] Test queue full, starvation, producer/consumer imbalance, and reconnect.
- [ ] Include repeated SD/WAV start/stop and EOF cycles with normal Gateway
      services active.
- [ ] Confirm no monotonic internal, DMA, or PSRAM loss.

## Acceptance

- [ ] No LVGL/network ownership leakage.
- [ ] No unbounded allocation or queue.
- [ ] No duplicate task/channel or use-after-free.
- [ ] Worst-case stack remaining >= 20%.
- [ ] A supported PCM WAV stored on SD plays end-to-end through
      `audio_manager` -> I2S TX -> speaker without loading the full file into
      memory.
- [ ] Unsupported/corrupt WAV, missing file, and SD read/unmount failures are
      bounded and recoverable without crash, leak, deadlock, or stale audio
      state.
- [ ] SD mount/card lifecycle remains outside `audio_manager`; the private WAV
      reader stays isolated from I2S so later source types can reuse the same
      manager-owned TX path.

---

# Sprint 12 — `esp_xiaozhi` Foundation

**Goal:** Pin, audit, activate, build, and lifecycle-test the managed component.

## Phase 12.1 — Dependency And License Audit

- [ ] Re-check the registry immediately before implementation.
- [ ] Pin exact version and commit `dependencies.lock`.
- [ ] Record license, IDF compatibility, dependencies, flash, IRAM, and DRAM
      delta.

## Phase 12.2 — Official Example And API Inventory

- [ ] Build an isolated target-hardware example.
- [ ] Reproduce official lifecycle before project adaptation.
- [ ] Document lifecycle, info, audio, event, MCP, and system-command APIs.
- [ ] Document callback context, pointer lifetime, ownership, and single-instance
      limits.
- [ ] Create API and dependency audit documents.

## Phase 12.3 — Service And Activation

- [ ] Validate `esp_xiaozhi_chat_get_info()` and release returned data.
- [ ] Document endpoint, device identity, activation flow, timeout, account, and
      error recovery.
- [ ] Audit component NVS persistence.
- [ ] Define private Xiaozhi keystore and reset policy.
- [ ] Never log activation secrets, tokens, or private payloads.

## Phase 12.4 — Side Effects And Storage

- [x] 12.4.2: Audit and disable Xiaozhi-owned system-time sync unless assigned.
- [x] 12.4.2: Review timeout, response size, UDP buffers, task stack/core/allocation and
      hello-message Kconfig.
- [x] 12.4.3: Audit NVS/cache-off execution paths and require every Xiaozhi
      lifecycle caller that may access NVS to use an internal-stack task.
- [x] 12.4.3: Keep the component NVS-operations service unregistered: static
      internal Xiaozhi audio stack removes the identified PSRAM-stack path.

## Phase 12.5 — Transport Decision

```text
get_info()
    -> WebSocket available: validate WebSocket only
    -> WebSocket unavailable: report unavailable; no MQTT fallback
```

- [x] P1/P2-C: Select and validate only the WebSocket control lifecycle on
      target hardware; MQTT+UDP is closed/not selected.
- [x] P2-D: Audit the resolved 0.1.2 public chat API. It has no arbitrary
      typed-text TX API; `CHAT_TEXT` receive callback plumbing uses bounded
      copied storage for USER and ASSISTANT roles.
- [x] P2.1: Add a temporary copied-status UI bridge and dedicated `XIAOZHI`
      screen for `DISCONNECTED`, `READY`, `LISTENING`, `PROCESSING`,
      `RESPONDING`, and `ERROR`. The existing GUI task owns the 100 ms local
      duration timer and cached transcript rendering; build is verified while
      LCD/P2-F interaction acceptance remains pending (P2-E serial lifecycle
      is accepted separately).
- [x] Phase 12 validation master feature gate: implemented with default `n`.
      It guards temporary ONLINE validation routing, Xiaozhi UI observer
      registration, and automatic `XIAOZHI` screen routing; P2-F sub-options
      depend on it. **IMPLEMENTED / BUILD VERIFIED.**
- [x] Feature-off compile regression: **BUILD VERIFIED.** With the master gate
      disabled, application composition makes no automatic Xiaozhi validation
      request, service probe, observer registration, or screen route.
- [x] Feature-off target-hardware behavior: **HARDWARE PASS 2026-08-25.** A
      clean gate-off image booted normal network/UI/cloud/audio services and
      Firebase for 120 seconds with no Xiaozhi worker, screen route,
      lifecycle/fault/P2 marker, panic, assert, or watchdog.
- [x] P2-E hardware acceptance: WebSocket `CONNECTED -> open -> observed
      OPENED -> bounded hold -> close -> observed CLOSED -> stop -> deinit ->
      MCP destroy`, stable cleanup, and `P2-E RESULT: PASS` captured on target.
- [ ] P2-F hardware acceptance: validation-only, fixed 16 kHz mono 60 ms Opus
      fixture infrastructure streams one embedded packet per send and records
      bounded USER/ASSISTANT text plus audio callback evidence. A lawful local
      fixture and target serial evidence are still required; do not add a
      private/raw typed-text path.
- [ ] Compare connection/reconnect, sockets, heap, CPU, audio loss, cleanup, and
      network failures.
- [x] Record the selected MVP transport in an ADR.

## Phase 12.6 — Fault Injection + Recovery + Cleanup Validation

- [x] Default-off repeated WebSocket lifecycle matrix: fresh context,
      EventGroup, handler, chat, MCP, generation, counters, and resource
      snapshots per cycle. **BUILD VERIFIED / prior target progression 1, 3,
      10, 20, and 100 recorded.**
- [x] Default-off controlled fault/recovery selector at safe project-owned
      continuation boundaries after get-info, MCP, EventGroup, chat init,
      handler registration, chat start, or audio-channel open. It preserves the
      first primary error, records cleanup errors separately, uses shared
      ordered cleanup, then runs a fresh P2-E recovery cycle. **BUILD VERIFIED.**
- [x] First target controlled fault `AFTER_CHAT_INIT`: expected injection,
      cleanup, fresh recovery, and aggregate fault summary passed; no captured
      panic/watchdog/assert/stale-callback or raw payload-tag marker.
- [x] Root-cause/resource acceptance: **HARDWARE PASS 2026-08-25.** The old
      sample was a contaminated pre-manager baseline. A-E attribution also
      found one-time fragmentation and a repeated ESP-IDF 6.0.1 cross-signed
      certificate-bundle leak. A source-hash-gated upstream-compatible build
      backport removed the Stage-D slope without disabling Firebase-required
      cross-signed verification. Post-fix 1/3/10/20/100 runs passed; cycle 100
      retained the 81920-byte largest-block plateau and ended +6392 Internal
      bytes versus the first settled sample.
- [x] Full controlled fault/recovery acceptance: `ALL_SUPPORTED` passed all
      seven safe boundaries with seven expected failures, seven cleanups,
      seven fresh P2-E recoveries, zero unexpected results, and stable t+5000
      resources.
- [ ] Real Wi-Fi/AP loss, Internet/DNS/TLS/service loss, server goodbye, remote
      timeout, malformed response, and allocation-pressure validation: source
      audited or target-hardware pending. Do not simulate them with Wi-Fi code,
      private transport calls, raw protocol messages, or unsafe lifecycle use.
- [ ] P2-F fault coverage: pending a valid lawful fixture and prior P2-F HIL
      proof; no fixture/audio workaround is authorized here.

## Acceptance

- [ ] Pinned component builds on ESP-IDF 6.0.1 and target hardware.
- [ ] Activation, storage, transport, and side effects are documented.
- [x] No post-warm-up lifecycle leak/fragmentation trend or duplicate
      resources across 100 target cycles after the proven upstream fix.
- [x] Feature-off restores pre-Xiaozhi behavior on target hardware.

---

# Sprint 13 — Project-Owned `voice_assistant`

**Goal:** Hide Xiaozhi behind project-owned lifecycle, state, policy, and events.

## Proposed States

```text
DISABLED
INITIALIZING
DISCONNECTED
CONNECTING
IDLE
LISTENING
THINKING
SPEAKING
ERROR
```

## Phases

### 13.1 Public API And State

- [ ] Define copied config/status and lifecycle, PTT, cancel, network, and status
      APIs without external types.
- [ ] Keep the Xiaozhi handle private and single-owner inside the project-owned
      foundation boundary.

### 13.2 Command Queue And State Machine

Commands: START, STOP, NETWORK_ONLINE, NETWORK_OFFLINE, BEGIN_LISTENING,
END_LISTENING, CANCEL, SHUTDOWN.

- [ ] Call Xiaozhi-facing APIs only through the owning foundation/worker context.
- [ ] Define valid transitions, finite timeouts/retries, and terminal failures.
- [ ] Use generation/epoch filtering for stale events.

### 13.3 Event Conversion

- [ ] Map connection, audio channel, text, TTS, emotion, goodbye, and errors.
- [ ] Copy callback-lifetime text, emotion, error source, and audio before
      callback return.
- [ ] Bound all payloads and keep callbacks short.

### 13.4 Network Integration

- [ ] Consume copied Wi-Fi facts only.
- [ ] Never own Station/provisioning lifecycle.
- [ ] Ensure voice recovery cannot disrupt cloud, reset, or provisioning.

### 13.5 Security Boundary

Default reject: reboot, OTA, NVS erase, Wi-Fi reconfiguration, provisioning,
arbitrary GPIO, shell/system commands.

## Acceptance

- [ ] Only `xiaozhi_foundation` directly depends on managed `esp_xiaozhi` provider types.
- [ ] `voice_assistant` uses the project-owned foundation API; no provider types escape.
- [ ] No external types in public headers.
- [ ] No callback ownership violations.
- [ ] Stale events rejected; repeated lifecycle creates no duplicate resources.

---

# Sprint 14 — Push-To-Talk Voice MVP

**Goal:** Complete reliable user speech -> Xiaozhi -> TTS playback.

## Phase 14.1 — Input And Privacy

- [ ] Use a dedicated PTT input where practical.
- [ ] Do not overload factory-reset long press.
- [ ] Show local listening indication.
- [ ] Transmit microphone audio only during authorized turns.
- [ ] Define mute, cancel, and timeout behavior.

## Phase 14.2 — Audio Uplink

- [ ] Feed `audio_manager` PCM through the approved OPUS/audio path.
- [ ] Freeze sample rate, channels, frame duration, bitrate, timestamps, queue
      depth, and ownership.
- [ ] Put PCM/encoded queues in PSRAM and DMA staging internally.
- [ ] Define bounded backpressure and drop policy.

## Phase 14.3 — Audio Downlink

- [ ] Copy callback-lifetime audio into a bounded PSRAM playback pool.
- [ ] Play only through `audio_manager`.
- [ ] Define jitter, queue full, underrun, cancel, and shutdown behavior.

## Phase 14.4 — Cancellation And Recovery

- [ ] Test release, double press, cancel while thinking, abort while speaking,
      Wi-Fi loss, timeout, queue full, and reconnect/new turn.
- [ ] Start half-duplex; full duplex is not an MVP requirement.
- [ ] Avoid overlapping Firebase TLS peaks with active voice when measured
      headroom is insufficient.

## Phase 14.5 — Limited Multi-Turn Conversation

- [ ] Keep a conversation open for a bounded interval after a reply.
- [ ] Initial wait target: 20-30 seconds, subject to measurement.
- [ ] Allow a follow-up PTT question in the same supported Xiaozhi session.
- [ ] Define maximum idle time, turn count, cancel, disconnect, and renewal.
- [ ] Keep bounded current-session transcript data.
- [ ] Close cleanly on timeout, reset, provisioning priority, network loss, or
      explicit cancel.

## Acceptance

- [ ] At least 100 consecutive voice turns.
- [ ] Audio transmitted only during authorized turns.
- [ ] No accidental factory reset or unbounded queue.
- [ ] Network recovery, existing services, and limited multi-turn work.
- [ ] No watchdog, stale session, or nominal overflow/underrun.

---

# Sprint 15 — GUI Voice And Interactive Chatbot

**Goal:** Add queue-driven status, transcript, emotion, and animated interaction.

## Phase 15.1 — Voice Screen Foundation

- [ ] Add animation, state, transcript, and error/network regions.
- [ ] Preserve the existing root-screen switching model.
- [ ] Keep reset, provisioning, and critical recovery above voice priority.

```text
RESET_RESULT
    > PROVISIONING
    > critical network recovery
    > VOICE
    > SENSOR_DASHBOARD
```

## Phase 15.2 — UI Model And Queue

- [ ] Add copied state, bounded UTF-8 text, bounded emotion, generation, and
      error model.
- [ ] Add a bounded/coalescing GUI voice queue.
- [ ] Avoid truncating inside UTF-8 code points.
- [ ] Reject stale generations.
- [ ] Only the GUI task touches LVGL.

## Phase 15.3 — State And Transcript

Render Offline, Connecting, Ready, Listening, Thinking, Speaking, and Error.

- [ ] Display copied user and assistant text.
- [ ] Define transcript size/lifetime/clearing and screen re-entry.
- [ ] Coalesce rapid events and use bounded error/session routing.

## Phase 15.4 — Interactive Chatbot Animation Runtime

Required state mapping:

```text
Disconnected -> offline animation
Connecting   -> connecting animation
Ready        -> idle animation
User talks   -> listening animation
Processing   -> thinking animation
Xiaozhi talks-> speaking animation
Error        -> error animation
```

Suggested assets:

```text
S:/voice/offline.gif
S:/voice/connecting.gif
S:/voice/idle.gif
S:/voice/listening.gif
S:/voice/thinking.gif
S:/voice/speaking.gif
S:/voice/error.gif
```

Optional allowlisted assets:

```text
S:/voice/happy.gif
S:/voice/sad.gif
S:/voice/speaking_happy.gif
S:/voice/speaking_sad.gif
```

Rules:

- [ ] Resolve animation only inside the GUI task.
- [ ] Never build paths from untrusted emotion strings.
- [ ] Convert emotion to a project-owned allowlisted enum.
- [ ] State has priority over emotion; reject stale emotion events.
- [ ] Do not reload unchanged GIFs.
- [ ] Coalesce updates and reject stale generations.
- [ ] Clear resources safely when leaving the voice screen.
- [ ] Restore newest copied state when re-entering.
- [ ] Prefer PSRAM for GIF frames and decoder work.
- [ ] Avoid per-frame allocation and repeated SD re-open.
- [ ] Start with 160x128 assets at roughly 8-15 FPS.
- [ ] Fall back GIF -> static image -> status text; animation failure must not
      fail voice.

Event flow:

```text
esp_xiaozhi callback
    -> xiaozhi_foundation copies/provider-adapts
    -> voice_assistant copies/validates product state
    -> project state/emotion model
    -> bounded app_gui queue
    -> GUI task animation resolver
    -> lvgl_image_handler
```

## Phase 15.5 — Performance And Hardware Closure

- [ ] Repeat IDLE -> LISTENING -> THINKING -> SPEAKING -> IDLE.
- [ ] Test emotion changes, disconnect, reset/provisioning preemption, missing or
      corrupt GIF, SD unmount, rapid updates, and screen enter/exit.
- [ ] Run at least 100 voice turns while monitoring decoder resources, PSRAM
      fragmentation, internal/DMA low-water, GUI responsiveness, and audio.

## Acceptance — Voice MVP Complete

- [ ] Correct voice state, transcript, and animation.
- [ ] Bounded allowlisted emotion mapping.
- [ ] No LVGL calls from non-UI callbacks.
- [ ] No stale animation/transcript after session change.
- [ ] Missing assets do not crash or terminate voice.
- [ ] No decoder leak, stale object, use-after-free, or nominal audio underrun.
- [ ] Provisioning/reset retain higher priority.

Sprint 15 closes the planned voice chatbot MVP:

```text
Push-to-Talk
+ Xiaozhi speech/LLM/TTS
+ bounded transcript
+ limited multi-turn
+ interactive state/emotion animation
```

---

# Sprint 16 — Audio Arbitration & Multi-Client Audio Policy — Complete

**Status:** Software complete / static review complete / build verified / bounded HIL accepted.

**Goal:** Preserve `audio_manager` as the sole I2S/DMA owner while allowing
multiple logical audio clients to request capture/playback through deterministic,
bounded policy.

Accepted policy summary:

```text
XIAOZHI      priority=70  CAPTURE=REJECT  PLAYBACK=QUEUE  interruptible=true
NOTIFICATION priority=50  PLAYBACK=QUEUE  interruptible=true
ALARM        priority=100 PLAYBACK=PREEMPT_LOWER_PRIORITY interruptible=false
```

- [x] Project-owned request metadata and deterministic GRANT/WAIT/REJECT/PREEMPT policy.
- [x] Separate bounded capture/playback arbiters above the existing manager.
- [x] Real manager state evidence required before ACTIVE ownership is claimed.
- [x] Unknown/legacy activity is never preempted without trusted owner metadata.
- [x] Notification/alarm policy integrated without transferring I2S/DMA ownership.
- [x] Bounded target arbitration matrix accepted.

Authoritative closure and HIL evidence live in:

```text
AI_Stored_Data/PHASE16_PROGRESS.md
AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md
AI_Stored_Data/PHASE16_HIL_EVIDENCE.md
```

Known deferred items include long-duration endurance, global cross-resource
fairness guarantees, dedicated arbiter stop/deinit APIs, and migration of
recorded-audio playback into arbitration.

## Phase 16.1 — Xiaozhi PCM Streaming Downlink — Complete

The accepted post-Phase-16 extension replaces full-response aggregation and
SD/WAV handoff with a bounded streaming path:

```text
WebSocket callback copy
-> bounded packet queue
-> Opus decode worker
-> bounded audio_manager-owned PCM16 PSRAM ring
-> audio_manager task
-> sole I2S/DMA playback
-> MAX98357A
```

- [x] PCM16 mono 16-kHz manager-owned ingress ring.
- [x] 7.68-second bounded capacity and 1.44-second normal prefill.
- [x] Finite packet-preserving backpressure rather than silent PCM drop.
- [x] Explicit bounded silence during transient post-start ingress gaps.
- [x] Automated target matrix PASS and audible recovery confirmed.
- [ ] Long-duration cloud/Firebase coexistence and endurance remain separate acceptance work.

Detailed record: `AI_Stored_Data/PHASE16_1_STREAMING_DOWNLINK.md`.

---

# Sprint 17 — MCP Read-Only Tools — Complete

**Goal:** Expose bounded, non-sensitive project status.

Implemented slices:

```text
smart_room.get_current_temperature_humidity
smart_room.get_cloud_sync_status
smart_room.get_system_status
```

All three slices are build verified and voice-HIL accepted by the user. Do not
add `network.get_status`: an offline device cannot receive a Xiaozhi MCP call.

Deferred candidates, subject to explicit scope approval:

```text
display.get_current_screen
```

- [x] Define exact schemas, timeout, errors, and stale semantics for all three
      implemented slices.
- [x] Read only through public snapshot APIs.
- [x] Bound JSON/text with fixed-size formatting buffers.
- [x] Exclude credentials, tokens, activation data, QR payloads, private NVS,
      files, and pointers.
- [x] Validate the three read-only tools through accepted user voice HIL.

## Acceptance

- [x] Schema and unavailable-result paths are bounded for every tool.
- [x] No direct driver access or sensitive disclosure.
- [x] No direct audio, cloud, sensor, or GUI ownership is taken by MCP.
- [x] User-confirmed voice HIL covers each implemented read-only tool.

---

# Sprint 18 — MCP Controlled Actions — In Progress

**Goal:** Add allowlisted side effects only for real project-owned actuators.

Approved actions:

```text
light.set_state
audio.stop_playback
audio.play_recorded / bounded allowlisted playback variant
cloud.push_latest
```

Execution:

```text
MCP request
    -> validate schema/range/auth/request ID
    -> project command queue
    -> owning manager
    -> physical/result confirmation
    -> bounded MCP response
```

- [x] Phase 18.1 defines a bounded light allowlist, validation, errors, and
      idempotent partial-state contract.
- [x] Phase 18.1 routes light control through the composition provider and
      `light_manager` owner.
- [x] Phase 18.1 reports the applied logical state through bounded MCP output.
- [x] Phase 18.1 target HIL accepted by the user on 2026-09-13.
- [ ] Define and validate the independent contracts for 18.2–18.4.

Always reject factory reset, credential erase, reboot, arbitrary OTA/GPIO, task
control, and shell/system commands.

## Phase 18.1 — Light control — Complete

- [x] `light.set_state`, `light.get_state`, and `light.get_capabilities` use
      bounded allowlists and owner-routed state snapshots.
- [x] Invalid field combinations are rejected deterministically.
- [x] Target light-control HIL accepted by the user on 2026-09-13.
- [x] Local light-manager control remains independent of MCP registration.

Phase 18 remains in progress. Phases 18.2–18.4 are not started and require an
explicit user request.

---

# Sprint 19 — Local Web Control V1: SD Card File Manager — Planned / Not Started

**Goal:** Deliver the SD-card-first local Web Control foundation with a
responsive PC/mobile file manager while preserving `sd_card_manager` ownership.

Planned scope includes browse/list, upload/drag-drop, download, delete, rename,
create/remove folder, SD usage/free-space reporting, bounded progress/error
reporting, WebSocket/event synchronization where justified, and one LCD Web
Remote screen with a Storage sub-view.

The Web UI is used after the device is already networked. It must not configure
or control Wi-Fi, provisioning, credentials, reconnect, or network lifecycle.
It must not expose filesystem paths outside the approved SD root.

Detailed scope and anti-drift rules:
`AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.

---

# Sprint 20 — Local Web Control V2: Playback + Volume — Planned / Not Started

**Goal:** Add SD-backed playback and volume control through existing audio owner
APIs without creating a second I2S/playback owner.

- [ ] Select supported SD audio from the storage frontend.
- [ ] Route playback/stop and supported controls through `audio_manager`.
- [ ] Route bounded volume state/control through the existing owner contract.
- [ ] Show copied playback state/error/result on Web/LCD frontend surfaces.
- [ ] Do not access I2S/DMA or create a duplicate playback state machine in the
      web layer.

Detailed scope: `AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.

---

# Sprint 21 — Local Web Control V3: Lights — Planned / Not Started

**Goal:** Expose the approved local light contract through a Web frontend while
keeping `light_manager` as the product state/effect/hardware owner.

- [ ] Support bounded on/off, color, brightness, and supported effects.
- [ ] Show copied state/capabilities and deterministic validation feedback.
- [ ] Reuse manager semantics rather than create Web-specific light state.
- [ ] Do not own NeoPixel/RMT/GPIO from HTTP/WebSocket callbacks.

Detailed scope: `AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.

---

# Sprint 22 — Local Web Control V4: Dashboard + System Status — Planned / Not Started

**Goal:** Add a consolidated read-mostly dashboard using safe project-owned
snapshots.

- [ ] Display approved sensor/storage/audio/light/cloud/system status facts.
- [ ] Connectivity may be displayed as read-only status only.
- [ ] No Wi-Fi configuration/control, provisioning, credential, reconnect, or
      lifecycle operations from Web UI.
- [ ] Rate-limit/coalesce updates and do not bypass managers for diagnostics.

Detailed scope: `AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.

---

# Sprint 23 — Local Web Control V5: Scenes + Logs + Diagnostics — Planned / Not Started

**Goal:** Complete the planned local Web surface with bounded manager-based
orchestration and sanitized support diagnostics.

- [ ] Scenes use approved manager/service APIs only.
- [ ] Logs/diagnostics are bounded and sanitized; do not expose credentials,
      tokens, PoP, activation/session secrets, private payloads, or unrestricted
      filesystem/NVS content.
- [ ] Keep OTA install/update, factory reset/credential erase, reboot, arbitrary
      GPIO/task/shell/system control outside this scope.

Detailed scope: `AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.

---

# Sprint 24 — Wake Word + Advanced Voice UX — Planned / Not Started

**Goal:** Evaluate and optionally add local wake word, VAD, privacy UX, and
advanced conversation after PTT, MCP, and the planned local-control roadmap are
stable.

This is the former Sprint 19 plan, deferred without dropping its content.

## Phase 24.1 — ESP-SR/WakeNet Feasibility

- [ ] Select and pin exact ESP-SR/WakeNet model/dependencies.
- [ ] Measure flash, PSRAM, internal/DMA, CPU, stacks, and continuous I2S.
- [ ] Validate coexistence with LCD, SD, Wi-Fi, cloud, and Xiaozhi.

## Phase 24.2 — Continuous Capture, Wake Word, And VAD

- [ ] Continuous local capture without continuous network transmission.
- [ ] Project-owned wake/VAD state flow.
- [ ] Visible microphone/listening and local mute indicators.
- [ ] Define false accept/reject, timeout, and re-arm.

## Phase 24.3 — Advanced Conversation

Optional measured features:

- [ ] Interrupt speaking through explicit abort.
- [ ] Continue bounded conversation after wake word.
- [ ] Evaluate feedback, AFE/AEC, then full duplex only if justified.

Do not add hidden always-on transmission, unbounded history, full duplex before
half-duplex closure, AEC before measurement, or unversioned models.

## Phase 24.4 — Endurance + HIL Closure

- [ ] At least 1,000 wake cycles.
- [ ] Eight-hour local always-on endurance.
- [ ] False accept/reject recorded.
- [ ] Mute/privacy verified.
- [ ] CPU, internal/DMA/PSRAM, stacks, thermal, and power documented.
- [ ] Reset/provisioning/critical recovery always preempt voice.
- [ ] No watchdog, leak trend, or uncontrolled audio failure.

No Sprint 24 implementation or HIL evidence is claimed by this roadmap move.

---

# Cross-Sprint Validation

Each Sprint 10-24 closure records, as applicable to that sprint:

- exact firmware revision and dependency lock;
- board, microphone, amplifier, speaker, power, and GPIO map where relevant;
- internal/DMA/PSRAM current, minimum, and largest values;
- CPU and task high-water marks;
- task/queue/handler/socket/decoder counts where observable;
- Wi-Fi loss/recovery and reset/provisioning preemption where relevant;
- audio overflow/underrun where relevant;
- sensitive-log review;
- hardware evidence and unresolved limitations.

Integrated fault cases include Wi-Fi loss during applicable voice states,
Internet or server outage, activation/auth rejection, transport reconnect,
cloud overlap, queue full/overflow, rapid PTT, cancel/abort, missing/corrupt GIF,
SD unmount, reset/provisioning preemption, 100-turn endurance, Local Web
storage/control error paths when those sprints start, and Sprint 24 eight-hour
endurance when enabled.

# Final Definition Of Done

The extension is complete only when dependencies build reproducibly; audio and
Xiaozhi lifecycles are independently accepted; ownership boundaries remain
intact; PTT and limited multi-turn are stable; transcript and animations are
queue-driven and hardware-accepted; MCP is bounded and allowlisted; planned
Local Web features are implemented/validated without taking manager ownership or
adding Wi-Fi configuration/control; wake word passes its separate Sprint 24
gate; feature-off preserves earlier accepted behavior; and no unfinished earlier
phase is silently marked complete.
