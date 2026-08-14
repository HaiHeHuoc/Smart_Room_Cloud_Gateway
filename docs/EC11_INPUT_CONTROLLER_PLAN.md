# EC11 Input Controller Integration Plan

**Status:** Planned / Not started  
**Placement:** After Sprint 13 (`voice_assistant` adapter), before Sprint 14 (Push-to-Talk MVP)  
**Target:** ESP32-S3 N16R8 / ESP-IDF 6.0.1  
**Purpose:** Add one EC11 rotary encoder with push button as a project-owned physical controller without coupling GPIO input directly to LVGL, audio, or `esp_xiaozhi`.

> This plan is an addendum to the existing project and Xiaozhi roadmaps. It does not reorder, replace, skip, or silently close existing Sprints 0-18. Sprint 12 remains an isolated Xiaozhi dependency/transport validation phase. Sprint 13 remains the project-owned `voice_assistant` boundary. The EC11 foundation is inserted between Sprint 13 and Sprint 14 so Push-to-Talk can consume a known-good project input path.

---

## 1. Updated Dependency Order

```text
Sprints 0-9 and pending acceptance
    -> Sprint 10 audio hardware validation
    -> Sprint 11 production audio_manager
    -> Sprint 12 esp_xiaozhi build and transport validation
    -> Sprint 13 project-owned voice_assistant adapter
    -> Phase 13.5 EC11 physical input controller foundation
    -> Sprint 14 push-to-talk MVP
    -> Sprint 15 GUI voice integration
    -> Sprint 16 MCP read-only tools
    -> Sprint 17 MCP controlled actions
    -> Sprint 18 wake word and advanced voice UX
```

Phase 13.5 is intentionally an inter-sprint foundation. It does not renumber or change the scope of Sprints 14-18.

---

## 2. Why EC11 Is Placed Here

### Not before or inside Sprint 12

Sprint 12 should keep its test environment narrow and answer one question: can the pinned `esp_xiaozhi` dependency build, connect, recover, and teardown reliably on the existing platform?

Adding a production encoder/input stack during Sprint 12 would add unrelated GPIO, debounce, interrupt/task, and UI-routing variables while Xiaozhi transport is being characterized.

An isolated EC11 hardware smoke test may be performed earlier if desired, but it must remain test-only and must not become production integration before Sprint 12 closes.

### After Sprint 13

Sprint 13 establishes the project-owned `voice_assistant` API and prevents `esp_xiaozhi` types from leaking into the application. Once that boundary exists, the input layer can emit application intents such as Push-to-Talk without calling Xiaozhi directly.

### Before Sprint 14

Sprint 14 is the first end-to-end Push-to-Talk voice phase. The EC11 push switch is a suitable physical PTT input, while rotary movement can later support menu navigation or volume control.

---

## 3. Hardware Baseline

The accepted Sprint 10 audio hardware and the Sprint 11 audio configuration remain unchanged.

The EC11 adds only independent GPIO inputs and must not modify the frozen microphone, amplifier, speaker, I2S pin map, sample format, or DMA policy.

### Candidate GPIO Reservation

```text
EC11 A / CLK    -> GPIO38
EC11 B / DT     -> GPIO39
EC11 SW         -> GPIO40
EC11 COM        -> GND
```

GPIO38/39/40 are candidate project pins, not a frozen hardware contract yet. Before implementation, verify that the exact board exposes these pins and that no board-level peripheral or later accepted project configuration owns them.

Recommended reserve after EC11 allocation:

```text
GPIO41 / GPIO42 -> spare project GPIO
```

Do not use flash/PSRAM-owned pins, strapping pins, active USB/JTAG pins, or any GPIO already owned by LCD, SD, DHT22, button, or audio.

---

## 4. Ownership And Architecture

The EC11 driver must report physical input facts only.

```text
EC11
  |
  v
encoder_manager
  |
  |  INPUT_ROTATE_CW
  |  INPUT_ROTATE_CCW
  |  INPUT_PRESS
  |  INPUT_RELEASE       (if required by PTT policy)
  v
application input routing
  |
  +--> GUI navigation / selection
  +--> volume intent
  +--> Push-to-Talk intent
```

### Ownership Rules

- `encoder_manager` owns EC11 GPIO setup, quadrature decoding, debounce/filtering, and copied input events.
- Application/input routing owns context-dependent meaning.
- `app_gui` remains the GUI owner.
- `audio_manager` remains the audio/I2S owner.
- `voice_assistant` remains the Xiaozhi lifecycle/protocol owner.
- `wifi_manager`, provisioning, NVS, cloud, and factory-reset ownership remain unchanged.

### Forbidden Coupling

`encoder_manager` must not directly:

- call LVGL;
- call `esp_xiaozhi` APIs;
- start/stop the voice service;
- open/close I2S;
- alter Wi-Fi lifecycle;
- write/erase NVS;
- trigger factory reset;
- reboot the system.

---

## 5. Phase 13.5 Breakdown

### Phase 13.5.1 — EC11 Hardware Bring-Up

**Goal:** Prove the physical encoder and push switch independently.

Tasks:

- Verify GPIO38/39/40 are exposed and electrically safe on the exact board.
- Wire EC11 A, B, SW and common ground.
- Configure appropriate pull-up/pull-down policy for the actual module.
- Detect clockwise rotation, counter-clockwise rotation, press, and release if needed.
- Characterize mechanical bounce and determine filtering/debounce values from hardware evidence.
- Confirm the encoder does not disturb LCD, SD, Wi-Fi, audio, button, sensor, or USB development workflow.

Acceptance:

- [ ] 100 clockwise detents are decoded without persistent direction errors.
- [ ] 100 counter-clockwise detents are decoded without persistent direction errors.
- [ ] Repeated button press/release is stable after debounce/filtering.
- [ ] No boot/strapping issue is introduced.
- [ ] No conflict with existing GPIO ownership.

### Phase 13.5.2 — Production `encoder_manager`

**Goal:** Add a small project-owned reusable input component.

Proposed structure:

```text
components/input/encoder_manager/
├── CMakeLists.txt
├── encoder_manager.c
├── include/encoder_manager.h
└── docs/README.md
```

Expected responsibilities:

- init/start/stop/deinit lifecycle;
- GPIO ownership;
- quadrature decoding;
- bounded debounce/filtering;
- copied project-owned events;
- finite queue/callback behavior;
- diagnostic counters for invalid transitions, dropped events, and button bounce where useful.

Acceptance:

- [ ] Repeated init/start/stop/deinit is clean and idempotent according to the API contract.
- [ ] No arbitrary task or callback calls LVGL directly.
- [ ] Queue-full/drop policy is bounded and documented.
- [ ] No unbounded allocation or ISR logging.
- [ ] Stack/heap impact is measured.

### Phase 13.5.3 — Application Input Routing

**Goal:** Convert physical events into context-dependent application intents without teaching the driver about screens, volume, or voice protocols.

Initial event model:

```text
INPUT_ROTATE_CW
INPUT_ROTATE_CCW
INPUT_PRESS
INPUT_RELEASE
```

Example policy:

```text
Normal UI
  CW       -> next
  CCW      -> previous
  PRESS    -> select

Audio context
  CW       -> volume up intent
  CCW      -> volume down intent
  PRESS    -> context-specific action

Voice/PTT context
  PRESS/hold or configured press policy -> PTT start intent
  RELEASE                               -> PTT stop intent
```

The final PTT press/hold/release semantics are owned by Sprint 14, not by `encoder_manager`.

Acceptance:

- [ ] Input routing has no dependency on raw `esp_xiaozhi` types.
- [ ] GUI updates still execute through the existing GUI/UI ownership path.
- [ ] Voice actions target only project-owned `voice_assistant` APIs.
- [ ] Audio volume/control targets only project-owned `audio_manager` APIs.

---

## 6. Sprint 14 Entry Gate

Sprint 14 Push-to-Talk may use EC11 only after:

- Sprint 12 transport validation is accepted;
- Sprint 13 `voice_assistant` boundary is accepted;
- EC11 hardware decoding is accepted;
- `encoder_manager` lifecycle and event delivery are accepted;
- application input routing is bounded and documented.

Reference flow:

```text
EC11 press/release
       |
       v
encoder_manager
       |
       v
application input routing
       |
       v
voice_assistant
       |
       v
esp_xiaozhi

MIC / speaker data remain owned by audio_manager.
```

---

## 7. Non-Goals

Phase 13.5 does not implement:

- Xiaozhi transport validation;
- production voice streaming;
- wake word;
- MCP tools;
- a GUI redesign;
- a second factory-reset path;
- direct EC11-to-LVGL control;
- direct EC11-to-Xiaozhi control;
- changes to the accepted Sprint 10/11 audio hardware configuration.

---

## 8. Rollback

The EC11 feature must remain removable without changing the accepted audio or voice transport foundation.

Rollback target:

```text
disable/remove encoder_manager
        -> remove EC11 input routing registration
        -> existing button, GUI, audio, network, cloud, and voice foundations continue unchanged
```

No persistent configuration migration should be required merely to remove EC11 support.

---

## 9. Final Acceptance

- [ ] Candidate GPIOs are physically verified on the target board.
- [ ] CW/CCW/press behavior is stable under realistic use.
- [ ] `encoder_manager` owns only physical input decoding.
- [ ] Application layer owns context-dependent action mapping.
- [ ] No direct LVGL/Xiaozhi/I2S/Wi-Fi/NVS calls from encoder code.
- [ ] Existing factory-reset button remains independent.
- [ ] Existing Sprint 10/11 audio hardware configuration remains unchanged.
- [ ] Resource impact and stack/heap headroom are documented.
- [ ] Sprint 14 can consume project-owned PTT intents without raw GPIO knowledge.
