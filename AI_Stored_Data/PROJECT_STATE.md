# Smart Room Cloud Gateway — AI Project State

Updated from branch: `phase/13-voice-assistant`
Snapshot date: 2026-08-25

## Working Constitution

- `AGENTS.md` is the repository-specific operating guide for AI-assisted work.
- Preserve completed roadmap history and current phase scope.
- Inspect implementation and documentation before editing.
- Keep changes minimal and evidence-based.
- Never claim build, hardware validation, merge, or runtime success without evidence.
- `AI_Stored_Data/` is cross-session support metadata only and may be deleted by Hải; firmware/build code must never depend on it.

## Current High-Level State

Sprint 12 software implementation and most prior target validation are complete, but selected HIL acceptance remains deferred because hardware is unavailable.

Sprint 13 `voice_assistant` software foundation is now complete on `phase/13-voice-assistant`.

Current status:

```text
Sprint 12  Software complete / selected HIL deferred
Sprint 13  Software complete / HIL pending
Sprint 14  NOT STARTED
```

## Sprint 12 — Deferred HIL Backlog

Keep these deferred until hardware is explicitly available:

- P2-F known-audio E2E from SD;
- BOOT `Starting...` regression acceptance;
- real AP/Wi-Fi/Internet/DNS/TLS/service-loss recovery;
- runtime resource/cleanup measurements.

Use:

- `AI_Stored_Data/PHASE12_HIL_TEST_PLAN.md`;
- `AI_Stored_Data/P2F_KNOWN_AUDIO_HIL.md`;
- `AI_Stored_Data/BOOT_STARTING_DEBUG.md`;
- dedicated branch `test/xiaozhi-p2f-known-audio-e2e`.

Do not repeatedly choose these as the next Codex task while hardware is unavailable.

## Sprint 13 — Voice Assistant Software Closure

Branch: `phase/13-voice-assistant`

Status: **SOFTWARE COMPLETE / BUILD NOT CLAIMED / HIL DEFERRED**

### Implemented

`components/application/voice_assistant/` now provides:

- project-owned conversation state machine;
- one long-lived orchestration task;
- bounded command queue and lock waits;
- session generation and stale-event rejection;
- long-lived Xiaozhi production-session integration through `xiaozhi_foundation`;
- real CONNECTED evidence for READY;
- explicit begin/end/recover lifecycle;
- intentional-stop vs unexpected-disconnect handling;
- copied audio status contract;
- latest-value audio callback coalescing;
- UI-safe copied status model;
- no direct LVGL, I2S, Wi-Fi, provisioning, cloud, reset, storage or arbitrary-driver ownership.

### Final ownership

```text
wifi_manager / app_network_coordinator
    -> Wi-Fi + provisioning/network lifecycle

audio_manager
    -> sole microphone/speaker/I2S/DMA/PCM owner

xiaozhi_foundation
    -> sole direct esp_xiaozhi/MCP/service/session boundary

voice_assistant
    -> conversation state/generation/command ordering/recovery policy
       + copied audio/transport orchestration

app_gui / ui_manager_lvgl
    -> GUI model/queue + sole LVGL ownership
```

### Production-vs-validation rule

The repository keeps Phase-12 validation code/assets for test use, but Phase-13 production defaults explicitly set:

```text
CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=n
```

Therefore normal Phase-13 firmware does not automatically request the temporary Phase-12 validator. Dedicated HIL/test branches may opt in explicitly.

Do not run the Phase-12 validation lifecycle and Phase-13 production session lifecycle concurrently.

### Why Phase 13 does not auto-start a conversation

Phase 13 provides the orchestration foundation only. It deliberately does not auto-call `voice_assistant_begin_session()` at boot.

Sprint 14 owns user-authorized Push-to-Talk, microphone uplink, Xiaozhi audio-channel transaction, response-audio playback, cancel/privacy behavior and the first real conversation trigger.

### Audio boundary

Current `audio_manager` does not expose a public live PCM streaming API. Do not bypass its private recording storage or create another I2S owner.

Sprint 14 must introduce the minimum bounded public streaming path required for:

```text
audio_manager -> voice_assistant -> xiaozhi_foundation
```

and response audio in the reverse direction while preserving `audio_manager` ownership.

### GUI/event boundary

Phase 13 owns copied scalar conversation/audio state only.

- PTT/audio transaction states become active in Sprint 14.
- transcript/emotion presentation and final GUI voice queue/rendering remain Sprint 15.
- do not pull those features backward merely to satisfy outdated roadmap wording.

### Security boundary

`voice_assistant` exposes no API for reboot, OTA, NVS erase/write, Wi-Fi reconfiguration, provisioning lifecycle, arbitrary GPIO/driver control or shell/system commands.

Voice recovery may clean only the Xiaozhi session it owns through the foundation boundary.

## Phase 13 HIL

Use `AI_Stored_Data/PHASE13_HIL_TEST_PLAN.md` when hardware becomes available.

Target evidence still pending includes:

- repeated production session start/stop;
- failed connect and explicit recovery;
- transport loss after READY;
- intentional-stop late callbacks;
- stale generation behavior;
- queue-pressure behavior;
- resource/stack trends;
- no panic/assert/WDT.

No target PASS is claimed yet.

## Next-work Guidance

When asked **“hiện tại nên làm gì tiếp theo trên Codex?”** while hardware is unavailable:

1. Do not choose the deferred Sprint-12 or Sprint-13 HIL backlog.
2. Treat Phase 13 as software complete unless a static/build issue is found.
3. The next software development task is **Sprint 14 — Push-To-Talk Voice MVP**.
4. Before coding Sprint 14, re-read `AGENTS.md`, `XIAOZHI_IMPLEMENTATION_ROADMAP.md`, `AI_Stored_Data/PHASE13_PROGRESS.md`, and the current `audio_manager` public API.
5. Preserve the ownership boundaries above.
6. Do not merge or delete the dedicated Phase-12 HIL assets merely because software development has moved forward.

## Verification Boundary

The Phase-13 final source diff was statically reviewed in this session. This environment did not run the ESP-IDF target toolchain or board, so the following are explicitly unclaimed:

- final `idf.py build` / link;
- target runtime behavior;
- production WebSocket HIL through `voice_assistant`;
- resource measurements;
- mic/Opus/TTS E2E.
