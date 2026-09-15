# AI_Stored_Data

## Purpose

This directory is the shared repository-local handoff store for AI-assisted work
on the Smart Room Cloud Gateway project. It synchronizes durable context between
ChatGPT conversations, Codex sessions, and other AI workflows that can read the
repository.

Use it for reviewed or explicitly agreed cross-session context such as:

- current project/phase state;
- important engineering decisions and constraints;
- accepted build/HIL/manual evidence;
- unresolved acceptance work and next actions;
- implementation details that future sessions need to resume safely.

## Authority

Hải authorizes AI assistants to create, overwrite, reorganize, and update files
inside `AI_Stored_Data/` when synchronizing project context.

This authorization applies only to this directory. Normal repository rules in
`AGENTS.md` still apply everywhere else.

The directory may be deleted at any time. Therefore:

- production firmware must never depend on it;
- CMake/Kconfig/build logic must never require it;
- runtime code must never read it;
- canonical implementation truth must remain in source and current project docs.

## Source-of-truth priority

When information conflicts, use:

1. Current source code and build configuration on the active branch.
2. `AGENTS.md` and current canonical repository documentation.
3. Explicit recent build/HIL/manual evidence from Hải.
4. Current handoff state in `AI_Stored_Data/`.
5. Historical records, conversation memory, or assumptions.

Do not silently resolve a conflict. Record the discrepancy and verify it.

## Update rules

Before significant implementation/review work:

1. Read `AGENTS.md`.
2. Inspect the active branch and relevant current source.
3. Read the relevant roadmap/component docs.
4. Read `PROJECT_STATE.md` and the phase-specific handoff file.
5. Inspect the real worktree before modifying production code.

After a meaningful decision, acceptance result, phase transition, integration,
or architecture/behavior change, update the relevant handoff file when it
materially helps future sessions.

Use these evidence labels consistently:

- `CONFIRMED` — supported by source, actual build/test logs, or explicit user
  acceptance.
- `PENDING` — not yet validated.
- `DECISION` — an agreed engineering choice.
- `ASSUMPTION` — temporary and not established fact.

Never store credentials, Wi-Fi passwords, PoP values, activation/session
secrets, transport tokens, private keys, service-account JSON, personal
secrets, or private payloads here.

## Current synchronization snapshot

Latest handoff synchronization: **2026-09-15**.

Active integration branch:

```text
main_including_Firebase_security
```

Current integration source baseline before the AI-state synchronization commits:

```text
b3b2e9b6f21ed355d6bdc7867ab184f73a4dd933
merge(audio): integrate Phase 18.2 playback and bounded selection
```

Current major state:

```text
Phase 16 / 16.1  accepted baseline; endurance/integration follow-up remains
Phase 17         COMPLETE / read-only MCP voice HIL accepted
Phase 18         MCP Controlled Actions / IN PROGRESS
Phase 18.1       COMPLETE / build PASS / target HIL accepted by Hải (2026-09-13)
Phase 18.2       SOFTWARE INTEGRATED / TARGET HIL PENDING
18.2.1           playback control + PTT suspension/auto-resume
                 software implemented / build + host tests verified / HIL pending
18.2.2           bounded playback start + voice SD audio selection
                 software implemented / build + host tests verified / HIL pending
Phase 18.3       NOT STARTED / scope preserved
Phase 18.4       NOT STARTED / scope preserved
Sprint 19        Local Web V1: SD Card File Manager / PLANNED / NOT STARTED
Sprint 20        Local Web V2: Playback + Volume / PLANNED / NOT STARTED
Sprint 21        Local Web V3: Lights / PLANNED / NOT STARTED
Sprint 22        Local Web V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23        Local Web V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24        Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

The Phase-18.2 merge is source-integration evidence only. It does not claim
hardware acceptance. Both 18.2.1 and 18.2.2 still require target HIL/resource
closure.

## Current application structure

```text
main/main.c
    -> smart_room_app
        -> smart_room_mcp_adapter
            -> xiaozhi_foundation
```

`main` is a thin entrypoint. `smart_room_app` owns product composition and
application policy. `smart_room_mcp_adapter` owns Smart Room provider
adaptation. `xiaozhi_foundation` remains the sole managed Xiaozhi/MCP
engine/session boundary.

`audio_manager` remains the sole I2S/DMA/playback owner and `sd_card_manager`
remains the SD/VFS lifecycle/lease owner. Phase-18.2 does not transfer these
resources into MCP, WebSocket callbacks, or application glue.

## Current Phase 18.2 surfaces

Phase 18.2.1:

```text
audio.control_playback { action: pause | resume | stop | restart }
audio.get_playback_state {}
```

Phase 18.2.2:

```text
audio.list_tracks {}
audio.play_track { track_id: exact-id }
audio.play_recorded {}
```

The SD audio catalog is bounded and rooted at `/sdcard/audio/`. MCP receives
logical track IDs/names/metadata only and never arbitrary model-supplied
filesystem paths. A persistent catalog worker owns bounded scanning and SD lease
use; callbacks use a copied cache.

The Phase-18.2 response path also contains generation/transport-fence hardening
after locally aborted Xiaozhi responses. Target evidence is still required to
prove stale speech/response isolation under real network timing.

## Current validation facts

Phase-18.1 acceptance includes explicit user confirmation of:

```text
Firebase boot             PASS
relevant source build     PASS
repeated PTT/TLS smoke    PASS
Phase-18.1 light HIL      PASS
```

Do not reinterpret this as a target run performed by an AI agent.

Phase-18.2 recorded software evidence includes:

```text
18.2.1 audio-manager host tests       PASS
18.2.1 voice-assistant host tests     PASS
18.2.1 Xiaozhi/provider host tests    PASS
18.2.1 ESP-IDF build                  PASS
18.2.1 target HIL                     PENDING / NOT CLAIMED

18.2.2 audio-manager host tests       PASS
18.2.2 voice-assistant host tests     PASS
18.2.2 Xiaozhi/provider host tests    PASS
18.2.2 ESP-IDF build                  PASS
18.2.2 target HIL                     PENDING / NOT CLAIMED
```

The 18.2.2 implementation record also captured firmware size `0x26fb90`, free
app partition `0x190470` (39%), and DIRAM `167376 / 341760` bytes (48.97%).
These are build/link evidence, not runtime heap/stack/HIL evidence.

Current retained voice/streaming facts include dynamic TLS buffers in PSRAM,
1 KiB outbound TLS records, a 20 KiB total/largest-contiguous PSRAM gate before
PTT, a 7.68-second streaming ingress ring, and 0.96-second normal prefill. A
delayed-first-PCM regression and longer endurance/resource checks remain
deferred outside Phase-18.2 target closure unless a newer target run explicitly
records them.

## Current documentation discrepancy

`XIAOZHI_IMPLEMENTATION_ROADMAP.md` still says Phases 18.2-18.4 are not started.
That execution status is older than the merged Phase-18.2 source and the newer
`PHASE18_2_PLAN.md`, `PHASE18_2_1_PROGRESS.md`, and
`PHASE18_2_2_PROGRESS.md` records.

For current execution status, source plus the newer Phase-18.2 records take
precedence. Preserve this discrepancy visibly until the canonical roadmap is
reconciled. Do not renumber or silently repurpose Phase 18.3/18.4 while doing
so.

## Future roadmap

The approved future Local Web roadmap remains SD-card-first:

```text
Sprint 19  Local Web V1: SD Card File Manager
Sprint 20  Local Web V2: Playback + Volume
Sprint 21  Local Web V3: Lights
Sprint 22  Local Web V4: Dashboard + System Status
Sprint 23  Local Web V5: Scenes + Logs + Diagnostics
Sprint 24  Wake Word + Advanced Voice UX
```

Web/LCD remain sibling frontends over existing manager/service boundaries. Web
UI does not configure/control Wi-Fi. Advanced OTA/factory-management remains
out of the current scope. The former Sprint 19 Wake Word plan is deferred to
Sprint 24.

## File index

### Current state / decisions

- `PROJECT_STATE.md` — primary current integrated snapshot, ownership, current
  Phase-18.2 state, validation status, future roadmap, and next-action guardrails.
- `PROJECT_STATE_CURRENT.md` — compact current-state synchronization companion.
- `DECISIONS.md` — durable project decisions and historical status overrides.
- `NEXT_WORK_AND_HIL_BACKLOG.md` — deferred validation and next-work routing.
- `LOCAL_WEB_DASHBOARD_PLAN.md` — approved Sprint 19-23 Local Web scope and
  Sprint 24 Wake Word deferral.
- `APPLICATION_STRUCTURE_CLEANUP.md` — integrated thin-main / application
  composition cleanup record.
- `COMPONENT_PORTABILITY_HARDENING.md` — earlier portability-hardening history
  and frozen architecture conclusions.

### Current Xiaozhi / voice phases

- `PHASE16_PROGRESS.md` — Phase-16 arbitration history/closure.
- `PHASE16_HIL_TEST_PLAN.md`
- `PHASE16_HIL_EVIDENCE.md`
- `PHASE16_HIL_TEST_BRANCH.md`
- `PHASE16_1_STREAMING_DOWNLINK.md`
- `PHASE17_XIAOZHI_SENSOR_ANSWER.md`
- `PHASE18_MCP_CONTROLLED_ACTIONS.md` — Phase-18 umbrella history; newer
  Phase-18.2 records supersede its stale pre-18.2 execution-status wording.
- `PHASE18_2_PLAN.md` — locked Phase-18.2 subdivision and evidence status.
- `PHASE18_2_1_DESIGN_AUDIT.md` — 18.2.1 owner/state/PTT design audit.
- `PHASE18_2_1_PROGRESS.md` — 18.2.1 implementation/build/host-test status and
  target HIL matrix.
- `PHASE18_2_2_PROGRESS.md` — 18.2.2 bounded catalog/playback implementation,
  stability hardening, build/host evidence, and target HIL matrix.

### Earlier phase records

- `PHASE13_PROGRESS.md`
- `PHASE13_HIL_TEST_PLAN.md`
- `PHASE13_ROADMAP_RECONCILIATION.md`
- `PHASE14_PROGRESS.md`
- `PHASE14_HIL_TEST_PLAN.md`
- `PHASE15_PROGRESS.md`
- `PHASE15_HIL_TEST_PLAN.md`

### Supporting review/debug records

- `FULL_PROJECT_REVIEW_TO_PHASE14.md`
- `BOOT_STARTING_DEBUG.md`
- `P2F_KNOWN_AUDIO_HIL.md`
- `CODEX_HIL_INDEX.md`

Historical files may intentionally contain terminology and pending state that
was true at the time they were written. Do not rewrite history merely to match
today's source. Use `PROJECT_STATE.md` for current execution truth and
phase-specific progress files for detailed evidence.

This directory remains support metadata only and is intentionally safe to
delete without changing firmware behavior.
