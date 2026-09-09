# Smart Room Cloud Gateway — Durable Decisions

This file records compact decisions that future AI sessions should preserve unless Hải explicitly changes them or newer repository evidence supersedes them.

## DECISION — AI handoff directory

Date: 2026-08-25

`AI_Stored_Data/` is the shared repository-local synchronization area for important AI handoff context across ChatGPT conversations, Codex sessions, and other AI-assisted workflows.

AI assistants are authorized by Hải to create, overwrite, reorganize, and update content inside this directory without treating those metadata edits as production architecture changes.

Constraints:

- the directory may be deleted by Hải at any time;
- firmware/build/runtime code must not depend on it;
- canonical source/docs remain higher authority;
- never store credentials, tokens, activation secrets, private payloads, or other sensitive data here.

## DECISION — Repository AI operating guide

Use `AGENTS.md` as the repository-specific operating guide before implementation/review work.

Important consequences:

- inspect before editing;
- preserve roadmap and completed phase history;
- stay inside requested phase scope;
- do not over-engineer;
- separate confirmed evidence from assumptions;
- do not claim build or hardware results that were not actually observed.

## DECISION — Xiaozhi ownership boundary

Only the project Xiaozhi boundary should directly depend on `esp_xiaozhi`; external Xiaozhi handles/types/pointers/transport objects must not leak into unrelated public component APIs.

Xiaozhi/network/audio callbacks must not directly own LVGL, Wi-Fi lifecycle, provisioning, project NVS reset, reboot, OTA, or arbitrary hardware actions.

## DECISION — Phase 12 transport

For Xiaozhi Phase 12, the selected project transport is **WebSocket only**.

If WebSocket is unavailable, report unavailable. Do not add MQTT+UDP fallback.

The upstream component may persist server-returned MQTT data internally, but the project does not select or expose MQTT as its Xiaozhi transport.

## DECISION — No typed-text workaround

Pinned/resolved `esp_xiaozhi` 0.1.2 does not expose an arbitrary typed-text TX API for the Phase 12 validation need.

Do not bypass that limitation with private/raw protocol calls or undocumented transport messages. P2-F must use a lawful audio fixture and public supported APIs.

## DECISION — Temporary validation isolation

Phase 12 validation infrastructure is not production voice-assistant behavior.

The master Kconfig validation gate remains default OFF. Normal Gateway behavior with the gate disabled must remain free of automatic Xiaozhi validation worker, validation screen route, observer registration, and transport-validation requests.

Production `voice_assistant` ownership/state-machine work belongs to Sprint 13 unless Hải explicitly changes the roadmap.

## DECISION — Roadmap continuity

Do not skip, replace, or silently close existing phases. New work must respect the established roadmap and acceptance state. Cleanup work must not silently become a new feature phase.

## DECISION — Component portability hardening is integrated and frozen

Date: 2026-09-09

The agreed `refactor/component-portability-hardening` work has been merged into
`main_including_Firebase_security` through merge commit
`b7ef51a87dcefa330cd0aa42e4d52dafe60f2bba`.

Preserve these conclusions:

- component dependency direction is `application -> service -> driver/framework`;
- domain folders are organizational containers; their direct children are
  ESP-IDF components;
- tightly coupled internal subsystems may live under parent-owned `modules/`
  directories and are not standalone ESP-IDF components by default;
- other components must not include another component's private module headers;
- reusable libraries, platform/service components, and product/application
  components have intentionally different reuse targets;
- product-specific coordinators/GUI/voice orchestration must not be generalized
  merely to increase a portability score;
- `board_config.h` remains the Smart Room physical hardware mapping authority;
- architecture and agreed refactor scope are frozen unless validation exposes a
  concrete defect.

The merge is complete even though full post-refactor automated and target-board
smoke acceptance may still be pending. Do not open a second portability wave to
solve an unproven problem; fix validated regressions narrowly.

## DECISION — Post-Phase-16 project stage

Date: 2026-09-09

Phase 16 audio arbitration is closed and Phase 16.1 streaming downlink has build
and bounded target acceptance recorded. The major feature-coding stage through
Phase 16 is complete.

Do not start Phase 17 or another major feature phase automatically.

Preferred work after remaining targeted acceptance gaps:

```text
full Gateway/Firebase + voice integration regression
-> confirmed bug fixes
-> hardening
-> performance/resource validation
-> documentation/release/portfolio closure
```

## DECISION — Voice roadmap numbering after Phase 16.1

Date: 2026-09-09

The old planned numbering that called MCP read-only "Sprint 16" is superseded.
Completed implementation history must not be renumbered. The canonical Version-2
sequence is now:

```text
Sprint 16   Audio Arbitration & Multi-Client Audio Policy       COMPLETE
Phase 16.1  Xiaozhi PCM Streaming Downlink                     COMPLETE
Sprint 17   MCP Read-Only Tools                                NOT STARTED
Sprint 18   MCP Controlled Actions                             NOT STARTED
Sprint 19   Wake Word And Advanced Voice UX                    NOT STARTED
```

Consequences:

- when Hải says "read-only MCP", treat it as **Phase/Sprint 17**;
- Phase/Sprint 18 is reserved for allowlisted MCP side effects;
- Phase/Sprint 19 is the wake-word/advanced-voice stage;
- do not reuse Phase 16 for MCP work or rewrite Phase-16/16.1 acceptance history;
- `XIAOZHI_IMPLEMENTATION_ROADMAP.md` is the canonical detailed Version-2
  roadmap for this numbering;
- if older historical roadmap text still contains the prior 16/17/18 mapping,
  treat that text as superseded numbering rather than current plan.
