# ESP32-S3 Smart Room Cloud Gateway — Xiaozhi Implementation Roadmap

**Status:** Approved syllabus / Not started  
**Target:** ESP32-S3 N16R8, ESP-IDF 6.0.1  
**Dependency baseline:** `espressif/esp_xiaozhi: "0.1.1"`  
**Voice MVP closure:** End of Sprint 15  
**Advanced voice closure:** End of Sprint 18

> This roadmap extends the existing project roadmap. It does not replace,
> reorder, skip, or silently close Sprints 0-9. Pending acceptance work in the
> original roadmap remains visible and higher priority unless explicitly
> deferred.

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
    -> Sprint 16 MCP read-only tools
    -> Sprint 17 MCP controlled actions
    -> Sprint 18 wake word and advanced voice UX
```

## 2. Ownership

```text
wifi_manager             owns Wi-Fi Station lifecycle and reconnect
provisioning_manager     owns temporary BLE provisioning transport
config_manager           owns persistent application configuration
cloud_manager            owns Firebase telemetry and retry policy
audio_manager            owns microphone, speaker, I2S, DMA staging and PCM flow
voice_assistant          owns esp_xiaozhi lifecycle and protocol adaptation
app_gui                  owns GUI models, queues, screens and animation selection
ui_manager_lvgl          owns LVGL runtime and synchronization
lvgl_image_handler       owns decoded image/GIF resources and active image objects
device/application APIs  own validated sensor and actuator operations
```

Only `voice_assistant` may include `esp_xiaozhi` headers. External handles,
enums, callback payloads, and transport objects must not leak into other public
component APIs.

No Xiaozhi, audio, network, or MCP callback may directly call LVGL, change
Wi-Fi lifecycle, start/stop provisioning, erase/write application NVS, reboot,
execute OTA, or control arbitrary hardware drivers.

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
      cleanup while its current stability task continues to select the proven
      recorded-PCM path.
- [x] Phase 11.4.2 connects this reader directly to the existing manager-owned
      TX path with bounded reads; no ring/task split is required without
      runtime evidence.

#### Phase 11.4.2 — Direct Bounded WAV Playback — Implemented / Build And Host-Test Verified

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
- [x] Aggregate logs expose WAV bytes read/streamed, maximum `fread()` latency,
      elapsed time, failures, and per-validation TX deltas without per-read
      INFO spam.
- [x] A default-off Kconfig proof plays one configured `/sdcard/...` WAV at
      task startup, then returns to the unchanged record/DSP/playback soak.
- [ ] Validate 5/30/60-second WAVs, invalid/removal cases, SD latency under
      Gateway load, clean EOF, sound quality, and golden-path regression on
      target hardware.

Reference flow:

```text
SD card
    -> sd_card_manager / mounted filesystem
    -> bounded WAV reader in the audio-manager task
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
- [x] Start with direct bounded read/decode/TX submission. Add a producer,
      consumer, ring, or backpressure scheduler only after measured SD latency
      and TX failure/gap evidence demonstrates a need.
- [x] Handle normal EOF, missing/unsupported/corrupt/truncated input, short
      reads, SD-unavailable state, TX errors, close errors, and defensive
      cleanup in the bounded proof path.
- [ ] Add user stop/cancel and production source arbitration in Phase 11.4.3.
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

- [ ] Audit and initially disable Xiaozhi-owned system-time sync unless assigned.
- [ ] Review timeout, response size, UDP buffers, task stack/core/allocation and
      hello-message Kconfig.
- [ ] Route NVS through an internal-stack service where PSRAM stacks would enter
      cache-off storage paths.

## Phase 12.5 — Transport Decision

```text
get_info()
    -> WebSocket only: validate WebSocket
    -> MQTT only: validate MQTT + UDP
    -> both: validate component-preferred path, then fallback
```

- [ ] Compare connection/reconnect, sockets, heap, CPU, audio loss, cleanup, and
      network failures.
- [ ] Record the selected MVP transport in an ADR.

## Phase 12.6 — Lifecycle Matrix

- [ ] Validate get_info -> init -> start -> connected -> stop -> deinit.
- [ ] Run at least 100 lifecycle cycles.
- [ ] Test Wi-Fi loss, repeated/invalid calls, partial failure, server goodbye,
      malformed response, and allocation failure.

## Acceptance

- [ ] Pinned component builds on ESP-IDF 6.0.1 and target hardware.
- [ ] Activation, storage, transport, and side effects are documented.
- [ ] No lifecycle leak trend or duplicate resources.
- [ ] Feature-off restores pre-Xiaozhi behavior.

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
- [ ] Keep the Xiaozhi handle private and single-owner.

### 13.2 Command Queue And State Machine

Commands: START, STOP, NETWORK_ONLINE, NETWORK_OFFLINE, BEGIN_LISTENING,
END_LISTENING, CANCEL, SHUTDOWN.

- [ ] Call Xiaozhi APIs only from the owning worker context.
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

- [ ] Only `voice_assistant` directly depends on `esp_xiaozhi`.
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
    -> voice_assistant copies/validates
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

# Sprint 16 — MCP Read-Only Tools

**Goal:** Expose bounded, non-sensitive project status.

Initial candidates:

```text
room.get_environment
network.get_status
system.get_status
display.get_current_screen
```

- [ ] Define exact schemas, timeout, errors, and stale semantics.
- [ ] Read only through public snapshot APIs.
- [ ] Bound JSON/text and prefer PSRAM where safe.
- [ ] Exclude credentials, tokens, activation data, QR payloads, private NVS,
      files, and pointers.
- [ ] Test concurrent calls and subsystem fault/coexistence.

## Acceptance

- [ ] Schema and error tests for every tool.
- [ ] No direct driver access or sensitive disclosure.
- [ ] No deadlock with audio, cloud, sensor, or GUI.
- [ ] Voice works when MCP tools are disabled.

---

# Sprint 17 — MCP Controlled Actions

**Goal:** Add allowlisted side effects only for real project-owned actuators.

Candidate actions:

```text
display.set_brightness
light.set_state
fan.set_state
servo.set_angle
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

- [ ] Define allowlist, ranges, timeout, errors, request ID, idempotency,
      duplicate suppression, and late-result policy.
- [ ] Route every action through the owning manager.
- [ ] Report actual execution results.
- [ ] Test malformed, repeated, out-of-range, unavailable, timeout, and
      concurrent requests.

Always reject factory reset, credential erase, reboot, arbitrary OTA/GPIO, task
control, and shell/system commands.

## Acceptance

- [ ] Every action is validated and owner-routed.
- [ ] Duplicate/late requests are deterministic.
- [ ] Physical state matches reported result.
- [ ] Local control remains functional with MCP disabled.

---

# Sprint 18 — Wake Word And Advanced Voice UX

**Goal:** Evaluate and optionally add local wake word, VAD, privacy UX, and
advanced conversation after PTT is stable.

## Phase 18.1 — ESP-SR/WakeNet Feasibility

- [ ] Select and pin exact ESP-SR/WakeNet model/dependencies.
- [ ] Measure flash, PSRAM, internal/DMA, CPU, stacks, and continuous I2S.
- [ ] Validate coexistence with LCD, SD, Wi-Fi, cloud, and Xiaozhi.

## Phase 18.2 — Continuous Capture, Wake Word, And VAD

- [ ] Continuous local capture without continuous network transmission.
- [ ] Project-owned wake/VAD state flow.
- [ ] Visible microphone/listening and local mute indicators.
- [ ] Define false accept/reject, timeout, and re-arm.

## Phase 18.3 — Advanced Conversation

Optional measured features:

- [ ] Interrupt speaking through explicit abort.
- [ ] Continue bounded conversation after wake word.
- [ ] Evaluate feedback, AFE/AEC, then full duplex only if justified.

Do not add hidden always-on transmission, unbounded history, full duplex before
half-duplex closure, AEC before measurement, or unversioned models.

## Acceptance

- [ ] At least 1,000 wake cycles.
- [ ] Eight-hour local always-on endurance.
- [ ] False accept/reject recorded.
- [ ] Mute/privacy verified.
- [ ] CPU, internal/DMA/PSRAM, stacks, thermal, and power documented.
- [ ] Reset/provisioning/critical recovery always preempt voice.
- [ ] No watchdog, leak trend, or uncontrolled audio failure.

---

# Cross-Sprint Validation

Each Sprint 10-18 closure records:

- exact firmware revision and dependency lock;
- board, microphone, amplifier, speaker, power, and GPIO map;
- internal/DMA/PSRAM current, minimum, and largest values;
- CPU and task high-water marks;
- task/queue/handler/socket/decoder counts where observable;
- Wi-Fi loss/recovery and reset/provisioning preemption;
- audio overflow/underrun;
- sensitive-log review;
- hardware evidence and unresolved limitations.

Integrated fault cases include Wi-Fi loss during all voice states, Internet or
server outage, activation/auth rejection, transport reconnect, cloud overlap,
queue full/overflow, rapid PTT, cancel/abort, missing/corrupt GIF, SD unmount,
reset/provisioning preemption, 100-turn endurance, and Sprint 18 eight-hour
endurance when enabled.

# Final Definition Of Done

The extension is complete only when dependencies build reproducibly; audio and
Xiaozhi lifecycles are independently accepted; ownership boundaries remain
intact; PTT and limited multi-turn are stable; transcript and animations are
queue-driven and hardware-accepted; MCP is bounded and allowlisted; wake word
passes its separate gate; feature-off preserves Sprints 0-9 behavior; and no
unfinished earlier phase is silently marked complete.
