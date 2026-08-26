# Full Project Review — Baseline through Phase 14

Updated: 2026-08-25
Reviewed branch: `phase/14-ptt-voice-mvp`
Baseline: `main_including_Firebase_security`
Purpose: architecture/ownership/risk checkpoint before further voice work or full-Gateway integration.

## Evidence boundary

- `phase/14-ptt-voice-mvp` is 127 commits ahead and 0 behind `main_including_Firebase_security` at this review.
- Earlier hardware-accepted behavior is preserved as documented in the canonical roadmap; this review does not re-claim hardware acceptance that has not been rerun on the Phase-14 branch.
- Phase 12 software is complete with selected HIL deferred.
- Phase 13 software is complete with HIL deferred.
- Phase 14 software is complete by static review, but ESP-IDF build and HIL remain pending.

## System evolution summary

### Foundation / original Gateway

The project already contains the established Gateway domains:

- LCD/ST7735 + LVGL UI;
- Wi-Fi station and reconnect;
- DHT22 sensor manager;
- Firebase authentication/cloud upload;
- NVS configuration;
- BLE provisioning;
- factory-reset input;
- SD-card ownership;
- performance/resource monitoring;
- audio manager with INMP441 RX and MAX98357 TX.

The original architectural rule remains valid: components communicate through owned public APIs/status/event paths; random tasks must not manipulate LVGL, I2S, storage lifecycle, Wi-Fi lifecycle, or cloud internals directly.

### Phase 12 — Xiaozhi foundation / transport validation

Phase 12 introduced `xiaozhi_foundation` as the sole project boundary around `esp_xiaozhi`, MCP and Xiaozhi transport/audio-channel APIs. It also added deterministic validation infrastructure including known-audio P2-F, startup/`Starting...` investigation and transport/resource validation.

Production rule:

```text
application
-> xiaozhi_foundation
-> esp_xiaozhi / MCP / transport
```

No other application component should gain a parallel direct Xiaozhi dependency.

Deferred acceptance remains on `test/xiaozhi-p2f-known-audio-e2e` via `RUN PHASE 12 HIL`.

### Phase 13 — voice session orchestration

Phase 13 added `voice_assistant` above `xiaozhi_foundation` for long-lived session lifecycle, generation filtering, bounded commands, intentional-stop semantics and explicit recovery.

Production rule:

```text
application intent
-> voice_assistant
-> xiaozhi_foundation
-> Xiaozhi transport
```

Phase-12 validation mode and Phase-13 production session must not own Xiaozhi concurrently.

Deferred acceptance remains on `test/phase13-voice-assistant-hil` via `RUN PHASE 13 HIL`.

### Phase 14 — Push-To-Talk Voice MVP

Phase 14 adds:

- dedicated PTT authorization policy;
- dedicated GPIO38 input, internal pull-down, active-high;
- live copied PCM16 stream contract from `audio_manager`;
- bounded mic uplink queue/task;
- production Xiaozhi audio-channel uplink;
- response callback/downlink aggregation;
- SD-managed temporary WAV handoff;
- speaker playback through the existing `audio_manager` TX owner;
- repeated-turn/failure serialization;
- production composition after `audio_manager` starts.

Current intended voice turn:

```text
GPIO38 press
-> PTT ARMING
-> real Xiaozhi READY
-> AUTHORIZED
-> INMP441 / audio_manager
-> PCM16 16 kHz mono
-> bounded uplink
-> Xiaozhi MANUAL listening
-> GPIO38 release
-> stop mic/listening, keep response channel
-> server response
-> bounded downlink + PSRAM aggregation
-> SD-managed WAV
-> audio_manager playback
-> MAX98357
-> playback IDLE
-> next turn allowed
```

## Ownership review

### GUI

`app_gui` / `ui_manager_lvgl` remain the LVGL ownership boundary. Network/audio/Xiaozhi callbacks must publish copied status/events rather than call LVGL directly.

### Network

`wifi_manager` owns Wi-Fi connection/reconnect. Firebase/cloud and Xiaozhi are clients of network availability; neither should independently reset or re-own Wi-Fi lifecycle as a normal recovery mechanism.

### Sensor / Firebase

Sensor sampling and Firebase upload remain independent FreeRTOS workloads. They may run while Xiaozhi is LISTENING/THINKING/SPEAKING. Their CPU/network work must remain bounded; concurrency itself is not an ownership violation.

### Storage

`sd_card_manager` remains the storage lifecycle/lease boundary. Phase-14 response WAV creation must keep the SD lease for the complete open/write/flush/close lifetime.

### Audio

`audio_manager` remains the sole I2S/DMA hardware owner for both INMP441 RX and MAX98357 TX. Voice code may request/copy audio through public contracts but must not create a second I2S reader/writer.

### Xiaozhi

`xiaozhi_foundation` remains the sole direct `esp_xiaozhi`/MCP boundary. `voice_assistant` owns application session/recovery orchestration; PTT/uplink/downlink layers sit above those boundaries.

### Physical buttons

- GPIO9 / `button_manager`: factory reset only.
- GPIO38 / `voice_assistant_ptt_gpio`: dedicated PTT only; GPIO48 remains NeoPixel-owned.
- Do not overload factory-reset long press with PTT.

## Concurrency review

Normal FreeRTOS concurrency is expected:

```text
sensor task
cloud/Firebase task
GUI task
Wi-Fi/event tasks
voice assistant tasks
Xiaozhi/network callbacks
audio manager task
```

These tasks may preempt/interleave. Correctness depends on resource ownership, bounded callbacks/queues and finite waits rather than trying to stop all unrelated tasks during a voice turn.

Existing Phase-14 protections:

- audio callbacks copy/enqueue and do not perform network I/O;
- Xiaozhi response callback copy/enqueues and does not perform speaker I/O;
- uplink/downlink queues are bounded;
- queue loss is observable;
- repeated PTT turns are serialized against prior response/playback;
- release-before-READY cannot authorize microphone capture;
- transport recovery is explicit and bounded;
- no unbounded reconnect loop.

## Important architectural gap found by this review

### General audio arbitration is not yet implemented

Current ownership prevents two components from directly owning I2S, but it does **not** yet provide a complete policy for multiple legitimate audio clients requesting the same `audio_manager` resource.

Examples:

```text
Xiaozhi SPEAKING
+ notification sound request

Xiaozhi LISTENING
+ another recorder request

critical alarm
+ Xiaozhi response playback
```

The required future distinction is:

```text
FreeRTOS tasks may run concurrently                 OK
Multiple clients may directly own I2S              NOT OK
Multiple clients may request audio_manager         needs arbitration policy
```

Recommended future audio-manager evolution:

```text
audio_manager
├── capture arbiter
│   ├── Xiaozhi
│   └── recorder/other capture client
└── playback arbiter
    ├── critical alarm
    ├── Xiaozhi response
    ├── notification
    └── UI/background sound
```

A future request model may include source, priority and interruptibility. Do not implement this only to close Phase 14; first prove the PTT MVP on hardware. Treat it as a post-Phase-14 architecture enhancement/integration requirement before multiple competing playback/capture clients are enabled.

## Network coexistence risk

Firebase HTTPS/cloud traffic and Xiaozhi WebSocket/audio traffic may run concurrently. This is architecturally allowed. HIL/integration must measure rather than assume acceptable behavior:

- uplink queue drops;
- response latency;
- TLS/heap pressure;
- Wi-Fi reconnect interaction;
- cloud retry interaction;
- watchdog/task starvation;
- Internal RAM/PSRAM/largest-block trend.

Do not pause Firebase globally during voice operation without evidence that coexistence is unsafe. If contention is proven, add a documented scheduling/rate-limit policy rather than hidden cross-component coupling.

## Review findings / priorities

### P0 — acceptance blockers, not necessarily code defects

1. Phase-14 branch has no verified ESP-IDF build evidence yet.
2. Phase-14 physical PTT/mic/uplink/downlink/speaker HIL has not run.
3. Actual Xiaozhi response codec is not proven. Current downlink assumes PCM16-compatible data after PCM negotiation; Opus target evidence requires a real decoder before playback can PASS.
4. Phase-12 and Phase-13 deferred HIL still need closure before final integration confidence.

### P1 — integration/architecture review items

1. Add general audio capture/playback arbitration before introducing competing non-Xiaozhi audio clients.
2. Run Firebase + Xiaozhi simultaneous-load regression and measure queue/resource behavior.
3. Review the Phase-14 source-scoped CMake redirect/tap after first real build; replace with a direct public integration point later if it becomes fragile or hard to maintain.
4. `session_generation` is a long-lived session identity, not a per-PTT-turn identity. Current safety relies on serialized turn windows; add a dedicated turn ID only if target evidence shows late cross-turn packets are possible.
5. Response playback is aggregate-to-PSRAM -> SD WAV -> playback. It is safe for current ownership but adds latency/SD dependency; consider direct streaming only after format/lifetime is proven.

### P2 — cleanup / maintainability

1. GPIO38 PTT assignment must be verified against the exact board and GPIO48 NeoPixel wiring.
2. Canonical roadmap/component documentation should continue to be reconciled as HIL closes; do not let `AI_Stored_Data` become the only source of important architecture facts.
3. Keep test/HIL harness commits on `test/...` branches; merge production branches only into the eventual full Gateway/Firebase integration branch.

## Recommended acceptance sequence

When hardware is available:

```text
1. Phase 12 HIL
   test/xiaozhi-p2f-known-audio-e2e
   RUN PHASE 12 HIL

2. Phase 13 HIL
   test/phase13-voice-assistant-hil
   RUN PHASE 13 HIL

3. Phase 14 HIL
   create/use test/phase14-ptt-voice-e2e-hil
   physical PTT -> mic -> Xiaozhi -> speaker + repeated turns

4. Integrate production history only into main_including_Firebase_security

5. Full Gateway regression
   Wi-Fi/provisioning + sensor + Firebase + GUI + SD + audio + Xiaozhi
   including simultaneous Firebase/Xiaozhi load
```

Production defects discovered by HIL belong on the owning production phase branch and must then be propagated forward/test branches. Test-harness defects stay on the test branch.

## Current conclusion

The project architecture through Phase 14 remains coherent: major hardware/service domains have explicit owners, and Phase 12-14 add Xiaozhi without intentionally bypassing Wi-Fi, LVGL, SD or I2S ownership. The largest newly identified architecture gap is not task scheduling itself but **audio request arbitration among multiple legitimate clients**. This is not required to claim Phase-14 software implementation complete, but it must be addressed before the project intentionally allows alerts/notifications/other recording clients to compete with active Xiaozhi audio.

No new runtime/build/HIL PASS is claimed by this review.
