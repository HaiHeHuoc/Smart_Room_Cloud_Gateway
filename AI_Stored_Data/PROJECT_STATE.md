# Smart Room Cloud Gateway — AI Project State

Updated from branch: `phase/12.5-transport-validation-clean`
Snapshot date: 2026-08-25

## Working Constitution

- `AGENTS.md` is the repository-specific operating guide for AI-assisted work.
- Preserve completed roadmap history and current phase scope.
- Inspect implementation and documentation before editing.
- Keep changes minimal and evidence-based.
- Never claim build, hardware validation, merge, or runtime success without evidence.

## Current Architecture Snapshot

The repository remains an ESP-IDF ESP32-S3 Smart Room Cloud Gateway with component-domain organization under `components/`.

Top-level domains currently include:

- application
- audio
- cloud
- connectivity
- display
- input
- sensing
- storage
- system
- ui

Phase 12 Xiaozhi work is isolated primarily behind:

`components/application/xiaozhi_foundation/`

The public boundary exposes copied, non-sensitive project-owned state rather than Xiaozhi-owned pointers/handles or transport secrets.

## Sprint 12 State

### CONFIRMED — dependency/foundation direction

- Target is ESP32-S3 N16R8 / ESP-IDF 6.0.1.
- Resolved managed dependency documented as `espressif/esp_xiaozhi: 0.1.2`, with manifest constraint `^0.1.1`.
- Xiaozhi transport decision is WebSocket only.
- MQTT+UDP is not selected as a project transport and must not be added as fallback during Phase 12.5.

### CONFIRMED — Phase 12 validation isolation

`CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE` is the default-off master gate for temporary Phase 12 runtime validation.

When the gate is disabled, normal Gateway application composition must not automatically request Xiaozhi validation, route the temporary Xiaozhi UI screen, or register its validation observer.

Temporary P2-F, lifecycle matrix, fault matrix, resource-attribution matrix, heap-trace options, and the new SD-fixture loader are subordinate validation-only options and are not production voice-assistant features.

### CONFIRMED — Phase 12.5

- P1/P2-C: WebSocket control lifecycle selected and validated on target hardware.
- P2-D: resolved 0.1.2 public API has no arbitrary typed-text TX API; receive transcript handling uses bounded copied USER/ASSISTANT text.
- P2.1: temporary copied-status UI bridge and `XIAOZHI` validation screen implemented; build verified. LCD interaction acceptance remains separate.
- Master validation feature gate implemented and build verified.
- Feature-off compile regression build verified.
- Feature-off target behavior passed on 2026-08-25 for a 120-second normal Gateway run with no Xiaozhi validation activity or watchdog/panic/assert evidence.
- P2-E target hardware acceptance passed: WebSocket CONNECTED -> open -> OPENED -> bounded hold -> close -> CLOSED -> stop -> deinit -> MCP destroy.
- MVP transport recorded in an ADR according to the roadmap.
- A project-generated lawful P2-F known-audio fixture now exists at `components/application/xiaozhi_foundation/test_assets/p2f_fixture.xzf` for the spoken phrase `What is two plus two?`.
- The fixture is a 6030-byte XZF1 stream with 33 Opus packets; local libopus verification confirmed 960 samples per packet at 16 kHz (60 ms).
- Validation-only SD fixture infrastructure now loads `/sdcard/xiaozhi/p2f_fixture.xzf` through the existing `sd_card_manager` lease before the existing P2-F worker runs.
- Dedicated hardware-test branch: `test/xiaozhi-p2f-known-audio-e2e`.

### PENDING — Phase 12.5

- P2-F known-audio E2E **target hardware acceptance is still pending**. The fixture/infrastructure now exist, but the new SD loader/test branch has not yet been ESP-IDF build-verified or run on target.
- Required P2-F target evidence remains: fixture READY, 33/33 TX packets, semantically correct USER transcript, non-empty ASSISTANT transcript, completed turn, server audio RX, clean transport teardown, and no panic/assert/WDT/resource regression.
- Do not create a private/raw typed-text workaround to bypass the supported audio/STT path.
- Remaining transport-comparison evidence includes connection/reconnect, sockets, heap, CPU, audio loss, cleanup, and network-failure behavior where not already covered by later Phase 12.6 evidence.

### CONFIRMED — Phase 12.6 evidence already present on this branch

Although the active branch name is Phase 12.5 cleanup, canonical roadmap/docs already record substantial Phase 12.6 validation:

- repeated WebSocket lifecycle matrix implemented/build verified;
- prior target progression through 1/3/10/20/100 cycles recorded;
- controlled safe-boundary fault/recovery framework implemented/build verified;
- first `AFTER_CHAT_INIT` controlled fault passed;
- root-cause/resource acceptance passed on 2026-08-25 after an upstream-compatible certificate-bundle fix removed the Stage-D retained-memory slope;
- full `ALL_SUPPORTED` controlled fault/recovery acceptance passed across seven safe boundaries.

### PENDING — Phase 12.6

- real Wi-Fi/AP loss;
- Internet/DNS/TLS/service loss;
- server goodbye / remote timeout / malformed response;
- allocation-pressure validation;
- P2-F fault coverage after P2-F HIL proof.

Do not simulate these with private transport calls, raw protocol messages, unsafe lifecycle manipulation, or unrelated Wi-Fi ownership changes.

## Public Xiaozhi Boundary

Current public API includes:

- copied service-info snapshot;
- WebSocket/AUTO transport request enum where AUTO resolves to WebSocket;
- non-blocking service-probe request;
- temporary transport-validation request;
- temporary copied validation-UI observer/status.

The temporary UI status contains bounded copied transcripts and scalar state only. It must not expose credentials, endpoints, tokens, raw audio, framework-owned pointers, or raw protocol payloads.

Callbacks must remain short and must not call LVGL directly.

## Current Debug/HIL Handoffs

- `AI_Stored_Data/BOOT_STARTING_DEBUG.md`: startup `Starting...` mitigation and later target-debug plan.
- `AI_Stored_Data/P2F_KNOWN_AUDIO_HIL.md`: P2-F SD fixture facts, expected logs, test procedure, and acceptance contract.

## Next-work guidance

Before implementing the next requested step:

1. Re-read the relevant Phase 12.5/12.6 section in `XIAOZHI_IMPLEMENTATION_ROADMAP.md`.
2. Inspect current `xiaozhi_foundation` implementation/config/docs and application composition in `main`.
3. Determine whether the request is cleanup/validation of existing infrastructure or truly belongs to the next phase.
4. Preserve WebSocket-only scope and validation-gate isolation.
5. Do not implement production `voice_assistant` Phase 13 behavior unless explicitly requested.
6. For P2-F target work, start from `test/xiaozhi-p2f-known-audio-e2e` and `AI_Stored_Data/P2F_KNOWN_AUDIO_HIL.md`.
7. Update this snapshot after material acceptance or architecture changes.

## Repository-local AI handoff rule

`AI_Stored_Data/` is allowed to be freely reorganized/updated by AI assistants as cross-session support metadata. It may be deleted by Hải and therefore must never become a firmware/build dependency.
