# Smart Room Cloud Gateway — Durable Decisions

This file records durable decisions and current status overrides that future AI
sessions should preserve unless Hải explicitly changes them or newer repository
evidence supersedes them.

## DECISION — AI handoff directory

Date: 2026-08-25

`AI_Stored_Data/` is the shared repository-local synchronization area for
important AI handoff context across ChatGPT conversations, Codex sessions, and
other AI-assisted workflows.

Hải authorizes AI assistants to create, overwrite, reorganize, and update
content inside this directory when synchronizing project context.

Constraints:

- the directory may be deleted by Hải at any time;
- firmware/build/runtime code must not depend on it;
- canonical source/docs remain higher authority;
- never store credentials, tokens, activation secrets, private payloads, or
  other sensitive data here.

## DECISION — Repository AI operating guide

Use `AGENTS.md` as the repository-specific operating authority before
implementation/review work, subordinate only to the LongAI project constitution
and higher-level safety/tool rules.

Consequences:

- inspect before editing;
- preserve roadmap and completed phase history;
- stay inside requested phase scope;
- do not over-engineer;
- separate confirmed evidence from assumptions;
- do not claim build, test, or hardware results that were not observed.

## DECISION — Xiaozhi ownership boundary

Only the project Xiaozhi boundary may directly depend on managed
`esp_xiaozhi`; external provider handles/types/pointers/transport objects and
credentials must not leak into unrelated public component APIs.

Xiaozhi/network/audio/MCP callbacks must not directly own LVGL, Wi-Fi lifecycle,
provisioning, project NVS reset, reboot, OTA, arbitrary GPIO, or hardware-driver
resources. Controlled MCP side effects must route through project-owned
providers/managers.

## DECISION — Phase 12 transport

For Xiaozhi Phase 12, the selected project transport is **WebSocket only**.

If WebSocket is unavailable, report unavailable. Do not add MQTT+UDP fallback.
The upstream component may retain server-returned MQTT data internally, but the
project does not select or expose MQTT as its Xiaozhi transport.

## DECISION — No typed-text workaround

Pinned/resolved `esp_xiaozhi` 0.1.2 does not expose an arbitrary typed-text TX
API for the Phase-12 validation need.

Do not bypass that limitation with private/raw protocol calls or undocumented
transport messages. P2-F uses lawful supported audio/public APIs.

## DECISION — Temporary validation isolation

Phase-12 validation infrastructure is not production voice-assistant behavior.
The master Kconfig validation gate remains default OFF. Normal Gateway behavior
with the gate disabled must remain free of the automatic validation worker,
validation screen route, observer registration, and transport-validation
requests.

## DECISION — Roadmap continuity

Do not skip, replace, renumber, or silently close established phases. New work
must respect the current roadmap and recorded acceptance state. Cleanup or
hardening must not silently become a new feature phase.

## DECISION — Component portability hardening is integrated and frozen

Date: 2026-09-09

The agreed `refactor/component-portability-hardening` work was merged into
`main_including_Firebase_security` through merge commit
`b7ef51a87dcefa330cd0aa42e4d52dafe60f2bba`.

Preserve these conclusions:

- dependency direction is `application -> service -> driver/framework`;
- domain folders are organizational containers and their direct children are
  ESP-IDF components;
- tightly coupled subsystems may live under parent-owned `modules/` and are not
  standalone components by default;
- other components must not include another component's private module headers;
- reusable libraries, platform/service components, and product/application
  components intentionally have different reuse targets;
- product-specific coordinators/GUI/voice orchestration must not be generalized
  merely to increase a portability score;
- `board_config.h` remains the physical Smart Room hardware-mapping authority;
- architecture/agreed refactor scope stays frozen unless validation exposes a
  concrete defect.

The merge being complete does not imply every post-refactor target regression or
endurance run has been executed. Fix validated regressions narrowly rather than
opening a second speculative portability wave.

## DECISION — Voice roadmap numbering after Phase 16.1

Date: 2026-09-09

Completed implementation history must not be renumbered. The canonical Version-2
sequence is:

```text
Sprint 16   Audio Arbitration & Multi-Client Audio Policy
Phase 16.1  Xiaozhi PCM Streaming Downlink
Sprint 17   MCP Read-Only Tools
Sprint 18   MCP Controlled Actions
Sprint 19   Wake Word And Advanced Voice UX
```

Consequences:

- "read-only MCP" means Sprint/Phase 17;
- Phase/Sprint 18 is reserved for allowlisted MCP side effects;
- Phase/Sprint 19 is wake-word/advanced-voice work;
- never reuse Phase 16 for MCP or rewrite Phase-16/16.1 acceptance history.

## STATUS OVERRIDE — Current roadmap execution state

Date: 2026-09-12
Supersedes only older **status values**, not the numbering decision above.

Current source/evidence state is:

```text
Sprint 16   COMPLETE / bounded HIL accepted
Phase 16.1  COMPLETE baseline / streaming HIL accepted / endurance pending
Sprint 17   COMPLETE / read-only MCP voice HIL accepted
Sprint 18   IN PROGRESS
Phase 18.1  IMPLEMENTED / current HEAD rebuild + HIL pending
Phase 18.2  NOT STARTED
Phase 18.3  NOT STARTED
Phase 18.4  NOT STARTED
Sprint 19   NOT STARTED
```

Older entries saying Sprint 17 or Sprint 18 are `NOT STARTED` are historical and
must not override this status.

At this synchronization point, `XIAOZHI_IMPLEMENTATION_ROADMAP.md` still has an
older top-level "Sprint 18 — Not Started" status. Treat that as a documented
canonical-doc discrepancy to reconcile, not as evidence that implemented source
should be ignored.

## DECISION — Phase 18 scope remains bounded

Date: 2026-09-12

Approved controlled-action sequence remains:

```text
18.1  light.set_state
18.2  audio.stop_playback
18.3  audio.play_recorded / bounded allowlisted playback variant
18.4  cloud.push_latest
```

`light.get_state` and `light.get_capabilities` are read-only companions to 18.1,
not additional controlled-action slices.

Do not add arbitrary GPIO, arbitrary filesystem playback, network reconnect,
factory reset, credential erase, reboot, OTA, shell/system command, or unrelated
actuator actions under Phase 18 without explicit scope approval.

Do not start 18.2 automatically merely because 18.1 source exists.

## DECISION — Phase 18.1 current source semantics

Date: 2026-09-12

Current source at production/source checkpoint `15cd0f06...` supersedes earlier
Phase-18.1 draft behavior:

- `power=off` cannot be combined with color, brightness, or effect;
- effect without explicit power is an activation request and turns the logical
  light ON;
- if that effect request omits color and preserved RGB is black, use white as a
  visible product fallback;
- other omitted fields preserve logical state;
- fixed product effect timing is owned by `light_manager`;
- current pulse duration is 1200 ms, not the earlier 300 ms value;
- MCP validates and routes through the composition/provider boundary;
  `light_manager` remains the product hardware owner.

Do not resurrect the older "effect-only preserves OFF" or 300 ms pulse semantics
unless Hải explicitly changes the product contract.

## DECISION — Evidence follows the commit that was actually validated

Date: 2026-09-12

A successful build/HIL on an earlier commit does not automatically validate a
later commit that modifies the affected path.

For current Phase 18.1 history:

- `f00e106...` explicitly records a full ESP-IDF build PASS for the light
  effects/capabilities checkpoint; HIL was pending.
- `e0255881...` explicitly records a clean build PASS for its TLS TX-record
  change; target boot/repeated PTT remained pending.
- `15cd0f06...` changes light, uplink/TLS, and streaming behavior but does not
  carry explicit build/HIL evidence.

Therefore current source HEAD must be rebuilt/revalidated before describing it
as build/HIL verified. Recommended order is clean build -> target boot/repeated
PTT -> Phase-18.1 light HIL -> affected streaming regression -> evidence update.
