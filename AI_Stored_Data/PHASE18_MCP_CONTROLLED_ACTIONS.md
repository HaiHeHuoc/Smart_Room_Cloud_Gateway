# Phase 18 — MCP Controlled Actions Scope

Status: **SCOPE FROZEN / NOT STARTED**

Updated: 2026-09-12
Integration branch: `main_including_Firebase_security`

## Goal

Phase 18 introduces a small, allowlisted set of MCP actions that can create
real device-side side effects through existing project ownership boundaries.
MCP is an orchestration/interface layer only; it must not directly own hardware,
I2S, GPIO, Wi-Fi, Firebase transport, LVGL, filesystem, or other lower-level
resources.

This document records Hải's approved Phase-18 scope and supersedes older generic
candidate lists for this phase where they conflict with the four actions below.

## Approved Scope

Phase 18 contains exactly these four controlled-action slices:

### 18.1 — NeoPixel light control

Preferred product action:

```text
light.set_state
```

Required capability:

- power on/off;
- allowlisted color selection;
- brightness percentage;
- bounded input validation;
- observable hardware result on the ESP32-S3 NeoPixel.

Example user intents:

```text
"Mở cho tôi đèn màu hồng, độ sáng 100"
"Mở cho tôi đèn màu xanh lá, độ sáng 20"
"Tắt đèn"
```

The MCP callback must not drive GPIO/NeoPixel hardware directly. A project-owned
light/NeoPixel manager or equivalent owning API must apply the hardware state.

### 18.2 — Stop audio playback

Preferred product action:

```text
audio.stop_playback
```

The action must route through the project-owned audio control boundary and
ultimately preserve `audio_manager` as the sole I2S/playback owner. It must
report deterministic busy/not-playing/error/success results without direct I2S
or DMA access from MCP.

### 18.3 — Allowlisted audio playback

Preferred initial action:

```text
audio.play_recorded
```

An allowlisted notification-playback variant may be used instead if the final
implementation review finds it better aligned with the existing product flow.
The MCP layer must not accept an arbitrary filesystem path or arbitrary raw
audio source from the model. Playback must route through project-owned audio
APIs and existing arbitration/ownership rules.

### 18.4 — Push latest cloud telemetry

Preferred product action:

```text
cloud.push_latest
```

This action requests an immediate upload of the latest bounded telemetry
snapshot through `cloud_manager`. If a new public API is required, add the
smallest bounded project-owned request API rather than letting MCP access
Firebase HTTP/auth/task internals directly.

The result must distinguish request acceptance from actual upload outcome when
those are asynchronous operations.

## Explicitly Out Of Scope

Phase 18 does **not** include:

- `network.reconnect`;
- display-status or display-brightness work merely to expand MCP count;
- fan control;
- servo control;
- arbitrary GPIO control;
- arbitrary WAV/file path playback;
- shell/system commands;
- arbitrary task control;
- factory reset;
- credential erase;
- arbitrary reboot;
- arbitrary OTA/update commands.

A later phase or explicit roadmap decision is required before adding any of
these capabilities.

## Architecture Contract

Every Phase-18 action follows:

```text
User intent
-> Xiaozhi / MCP tool
-> schema + allowlist validation
-> project-owned action/provider boundary
-> owning manager/service
-> real device/service operation
-> bounded result
-> MCP response
```

Rules:

1. MCP must never bypass the owning manager/service.
2. Inputs are bounded and validated before side effects.
3. Unsupported values are rejected deterministically.
4. Busy/error/timeout behavior must be explicit.
5. Duplicate/idempotency behavior must be defined where repeated delivery can
   cause an undesirable duplicate side effect.
6. Command acceptance must not be reported as physical/service success unless
   the owner can actually prove completion.
7. No direct LVGL calls from MCP callbacks.
8. No provider handles, driver handles, DMA buffers, file handles, credentials,
   or private subsystem pointers may escape their owners.
9. Existing Phase-16 audio arbitration remains authoritative for audio resource
   ownership and preemption.

## Recommended Implementation Order

```text
18.1 light.set_state
-> 18.2 audio.stop_playback
-> 18.3 audio.play_recorded / allowlisted notification playback
-> 18.4 cloud.push_latest
-> END PHASE 18
```

Do not add a fifth controlled action merely to increase feature count.

## Acceptance Direction

Each slice requires at least:

- source/architecture review;
- ESP-IDF build verification;
- bounded invalid-input/error tests where practical;
- target HIL confirming the intended side effect;
- regression check that existing Phase-17 read-only MCP and core Gateway/voice
  behavior remain functional.

For NeoPixel HIL specifically, acceptance should include at minimum:

```text
pink, brightness 100%
green, brightness 20%
off
rapid color/brightness changes
boundary brightness values
```

Phase 18 must not be marked complete until all four approved slices have their
required implementation and acceptance evidence.
