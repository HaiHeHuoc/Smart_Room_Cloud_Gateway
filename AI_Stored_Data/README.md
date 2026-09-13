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

Latest handoff synchronization: **2026-09-13**.

Active integration branch:

```text
main_including_Firebase_security
```

Current production/source baseline before the documentation-only synchronization
that followed it:

```text
0a8c83f7776d8259f22208a66f7fc4bd52156aff
Cleanup code structure
```

Current major state:

```text
Phase 16 / 16.1  accepted baseline; endurance/integration follow-up remains
Phase 17         COMPLETE / read-only MCP voice HIL accepted
Phase 18         IN PROGRESS
Phase 18.1       COMPLETE / build PASS / target HIL accepted by Hải (2026-09-13)
Phase 18.2-18.4  NOT STARTED
Phase 19         NOT STARTED
```

Current application structure:

```text
main/main.c
    -> smart_room_app
        -> smart_room_mcp_adapter
            -> xiaozhi_foundation
```

`main` is now a thin entrypoint. `smart_room_app` owns product composition and
application policy. `smart_room_mcp_adapter` owns Smart Room provider
adaptation. `xiaozhi_foundation` remains the sole managed Xiaozhi/MCP
engine/session boundary.

The application-structure cleanup is integrated on the active branch, not an
unmerged refactor. See `APPLICATION_STRUCTURE_CLEANUP.md`.

## Current validation facts

Phase-18.1 acceptance includes explicit user confirmation of:

```text
Firebase boot             PASS
relevant source build     PASS
repeated PTT/TLS smoke    PASS
Phase-18.1 light HIL      PASS
```

Do not reinterpret this as a target run performed by an AI agent.

The later application-structure cleanup recorded a normal ESP-IDF build PASS but
no new target HIL specific to the structural move. Earlier Phase-18.1 acceptance
and the cleanup build remain separate evidence.

Current implementation facts that supersede older notes include:

- dynamic mbedTLS buffers in PSRAM;
- 1 KiB outbound TLS record;
- 20 KiB total/largest-contiguous PSRAM gate before PTT;
- 7.68-second streaming ingress ring;
- 0.96-second normal streaming prefill;
- prefill timeout begins after first PCM, not at `TTS_START`;
- Phase-18.1 effect-only activation and visible-white fallback semantics;
- pulse effect timing is 1200 ms, not the earlier 300 ms value.

A delayed-first-PCM streaming regression and longer endurance/resource checks
remain deferred and do not reopen Phase 18.1.

## File index

### Current state / decisions

- `PROJECT_STATE.md` — current integrated snapshot, ownership, accepted state,
  deferred validation, and next-action guardrails.
- `PROJECT_STATE_CURRENT.md` — synchronization companion created during the
  2026-09-13 documentation reconciliation; `PROJECT_STATE.md` remains the
  primary current handoff.
- `DECISIONS.md` — durable project decisions and historical status overrides.
- `NEXT_WORK_AND_HIL_BACKLOG.md` — current deferred validation and next-work
  routing.
- `APPLICATION_STRUCTURE_CLEANUP.md` — integrated thin-main / application
  composition cleanup record.
- `COMPONENT_PORTABILITY_HARDENING.md` — earlier portability-hardening history
  and frozen architecture conclusions. Newer current-state files supersede any
  pre-integration wording inside its historical sections.

### Current Xiaozhi / voice phases

- `PHASE16_PROGRESS.md` — Phase-16 arbitration history/closure.
- `PHASE16_HIL_TEST_PLAN.md`
- `PHASE16_HIL_EVIDENCE.md`
- `PHASE16_HIL_TEST_BRANCH.md`
- `PHASE16_1_STREAMING_DOWNLINK.md`
- `PHASE17_XIAOZHI_SENSOR_ANSWER.md`
- `PHASE18_MCP_CONTROLLED_ACTIONS.md` — current approved Phase-18 scope,
  finalized Phase-18.1 contract/acceptance, and 18.2-18.4 NOT STARTED state.

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

Historical files may intentionally contain the terminology and pending state
that was true at the time they were written. Do not rewrite history merely to
match today's source. Use `PROJECT_STATE.md` for current truth.

This directory remains support metadata only and is intentionally safe to
delete without changing firmware behavior.
