# Local Web Dashboard Plan

Updated: 2026-09-13
Status: **APPROVED ROADMAP / PLANNED / NOT STARTED**
Active integration branch: `main_including_Firebase_security`

## Purpose

This document is the durable cross-session plan for the Local Web Control
Dashboard. Future ChatGPT/Codex/AI sessions must use it to avoid roadmap drift
unless Hải explicitly approves a later roadmap change.

This is planning documentation only. It does not claim that any web-dashboard
implementation, build, target run, or HIL evidence exists.

## Roadmap position

Sprint 18 remains **MCP Controlled Actions** with its existing Phase 18.x
numbering, scope, history, and acceptance evidence unchanged.

The approved post-Sprint-18 sequence is:

```text
Sprint 19  Local Web Control V1: SD Card File Manager     PLANNED / NOT STARTED
Sprint 20  Local Web Control V2: Playback + Volume       PLANNED / NOT STARTED
Sprint 21  Local Web Control V3: Lights                  PLANNED / NOT STARTED
Sprint 22  Local Web Control V4: Dashboard + System Status
                                                         PLANNED / NOT STARTED
Sprint 23  Local Web Control V5: Scenes + Logs + Diagnostics
                                                         PLANNED / NOT STARTED
Sprint 24  Wake Word + Advanced Voice UX                 PLANNED / NOT STARTED
```

The former Sprint 19 Wake Word / Advanced Voice UX plan is deferred to Sprint
24. Its content and required order are preserved in
`XIAOZHI_IMPLEMENTATION_ROADMAP.md`.

## Product direction

The web roadmap is **SD-card-first**.

V1 exists primarily to make the device's SD-backed content manageable from a
PC or phone on the same local network. Later versions add playback, lights,
status, scenes, logs, and diagnostics without turning the web frontend into a
new domain owner.

The Web UI and LCD are sibling frontends over the same project-owned managers
and services:

```text
Browser / Local Web UI              LCD / app_gui
          |                              |
          +----------- frontend ---------+
                         |
               project-owned APIs
                         |
       managers / services / coordinators
                         |
                 drivers / hardware
```

Neither frontend may bypass an owning manager/service to manipulate I2S, SD
mount lifecycle, NeoPixel/RMT, Wi-Fi lifecycle, NVS, cloud transport, LVGL, or
other driver/framework resources directly.

## Fixed boundaries

### No Wi-Fi configuration/control in Web UI

The Local Web Dashboard is used **after the device is already networked**.
Do not add web flows that:

- configure SSID/password;
- start/stop provisioning;
- connect/disconnect/reconnect Wi-Fi;
- erase Wi-Fi credentials;
- expose provisioning PoP/QR/session material.

Read-only connectivity/system status may be displayed in Sprint 22, but the Web
UI does not own or control Wi-Fi lifecycle.

### Advanced OTA/factory management remains out of scope

Do not add through Sprints 19-23:

- OTA upload/install/update orchestration;
- factory reset or credential erase;
- reboot/power-control management;
- arbitrary NVS editing;
- arbitrary GPIO/task/shell/system commands.

Those require separate explicit scope and safety review if ever approved.

### Existing ownership remains authoritative

Examples of required routing:

- storage/file operations -> `sd_card_manager` / approved storage APIs;
- audio playback/volume -> `audio_manager` / approved audio APIs;
- light changes -> `light_manager`;
- sensor facts -> `sensor_manager` snapshots;
- cloud facts/actions -> `cloud_manager` public APIs;
- GUI state -> `app_gui` queue/model APIs;
- network facts -> existing copied/snapshot status APIs only.

If a required public API does not exist, design the narrow owner-preserving API
first. Do not create a shortcut from the web server to a driver.

## Sprint 19 — Local Web Control V1: SD Card File Manager

Status: **PLANNED / NOT STARTED**

### Goal

Deliver a responsive local PC/mobile file-management frontend for the mounted SD
card before adding other web-control features.

### Planned user capabilities

- browse/list directories;
- show file/folder metadata suitable for the UI;
- upload files with bounded progress/error reporting;
- drag-and-drop upload on capable desktop/mobile browsers;
- download files;
- delete files with explicit confirmation/error reporting;
- rename files/folders;
- create and remove folders;
- show SD capacity, used space, and free space;
- keep the UI responsive on PC and mobile;
- use a bounded WebSocket/event channel where it materially improves progress
  and state synchronization;
- add one LCD **Web Remote** screen with a Storage sub-view/status surface.

### Required engineering constraints

- SD-card-first: storage is the first web-domain integration.
- Never expose paths outside the approved SD mount/root.
- Validate/canonicalize paths and reject traversal/escape attempts.
- Bound request sizes, path lengths, filenames, queue depth, and temporary
  buffers.
- Do not load large files wholly into internal RAM/PSRAM merely for transfer.
- Preserve `sd_card_manager` mount/VFS/lease ownership and existing concurrent
  users such as LVGL/audio.
- Define behavior for card unavailable/unmounted, file changes during transfer,
  duplicate names, partial upload, cancellation, low/free-space failure, and
  disconnect during an operation.
- Do not expose credentials, tokens, private NVS data, or unrelated filesystem
  content.

### Acceptance direction when implementation starts

Implementation closure must include source/build evidence and target/HIL evidence
appropriate to storage/network concurrency. This planning document itself is
not evidence. At minimum, future acceptance should cover representative
browse/upload/download/delete/rename/folder flows, invalid/path-traversal cases,
large/bounded transfers, interruption/recovery, SD failure/remount behavior,
and coexistence with existing Gateway services.

## Sprint 20 — Local Web Control V2: Playback + Volume

Status: **PLANNED / NOT STARTED**

### Goal

Control existing SD-backed audio playback from the web frontend without creating
a second audio owner.

### Planned scope

- select a supported audio file from the storage view;
- start playback through `audio_manager`;
- stop playback and expose other controls only if the manager contract supports
  them at implementation time;
- set/display bounded playback volume through the existing owner API;
- show copied playback state/error/result;
- add/update the LCD Web Remote playback sub-view as appropriate.

### Boundaries

- no direct I2S/DMA access from HTTP/WebSocket handlers;
- no duplicate playback task/state machine in the web layer;
- no arbitrary filesystem playback outside the approved SD root;
- preserve existing arbitration and owner semantics.

## Sprint 21 — Local Web Control V3: Lights

Status: **PLANNED / NOT STARTED**

### Goal

Expose the already project-owned light contract through a local web frontend.

### Planned scope

- on/off;
- supported color selection;
- brightness;
- supported effects;
- copied current state/capabilities;
- clear validation/error feedback;
- LCD Web Remote light sub-view as appropriate.

### Boundaries

Route mutations through `light_manager` and the same validated product semantics
used by other frontends. The web layer must not own NeoPixel/RMT/GPIO or create
a conflicting light-state model.

## Sprint 22 — Local Web Control V4: Dashboard + System Status

Status: **PLANNED / NOT STARTED**

### Goal

Add a consolidated read-mostly operational dashboard over existing snapshots.

### Planned scope

Where public project APIs already provide the data safely, display items such
as:

- sensor values/staleness;
- SD availability/capacity;
- audio/light summary state;
- cloud synchronization state;
- read-only network/connectivity facts;
- uptime/resource/system-health facts that are already exposed safely.

### Boundaries

- network data is **status only**; no Wi-Fi configuration/control;
- no raw secret/config/NVS dump;
- do not bypass managers to collect diagnostics;
- rate-limit/coalesce updates so the dashboard does not become a new load or
  timing hazard.

## Sprint 23 — Local Web Control V5: Scenes + Logs + Diagnostics

Status: **PLANNED / NOT STARTED**

### Goal

Complete the planned local-control surface with bounded orchestration and useful
support diagnostics.

### Planned scope

- scenes composed only from approved owner APIs;
- logs exposed through a bounded/sanitized read path;
- diagnostics/status views useful for debugging and demo;
- safe export/download of approved diagnostic information where justified;
- LCD Web Remote scene/diagnostic status as appropriate.

### Boundaries

- scenes orchestrate managers; they do not become driver owners;
- logs must not expose credentials, tokens, PoP, activation/session secrets,
  private payloads, or unrestricted filesystem content;
- diagnostics are bounded and primarily read-only;
- no OTA, factory reset, credential erase, reboot, shell, arbitrary GPIO, or
  unrestricted NVS management.

## Sprint 24 — Wake Word + Advanced Voice UX

Status: **PLANNED / NOT STARTED**

Sprint 24 is the deferred former Sprint 19 wake-word plan. Preserve this order:

```text
24.1  Feasibility / ESP-SR-WakeNet dependency and resource audit
24.2  Continuous local capture + WakeNet/wake word + VAD
24.3  Advanced conversation / richer voice UX after wake-word stability
24.4  Endurance + HIL / privacy, false-wake, resource and coexistence closure
```

Detailed wake-word content remains canonical in
`XIAOZHI_IMPLEMENTATION_ROADMAP.md`. Moving it to Sprint 24 does not claim that
any WakeNet/VAD model has been integrated or that any target evidence exists.

## Cross-sprint frontend rules

For Sprints 19-23:

1. Keep browser/LCD work at the presentation/application edge.
2. Use existing manager/service ownership and copied snapshots/commands.
3. Prefer bounded queues, finite timeouts, bounded payloads, and explicit error
   states.
4. Keep large transfers streaming/bounded; do not consume internal/DMA memory
   unnecessarily.
5. Sanitize all user-controlled paths/names/parameters.
6. Do not log or return secrets/private configuration.
7. Preserve normal Gateway behavior when the local web feature is disabled or
   unavailable.
8. Do not claim build, target, HIL, endurance, or security acceptance before it
   is actually executed and recorded.

## Scope-change rule

This file records the approved roadmap, not permission to implement every item
immediately. Each sprint remains **PLANNED / NOT STARTED** until Hải explicitly
starts it. Any material change to sprint numbering, Wi-Fi scope, owner
boundaries, destructive/OTA features, or Wake Word placement requires explicit
approval and synchronization of the canonical roadmap/current-state/decision
documents.
