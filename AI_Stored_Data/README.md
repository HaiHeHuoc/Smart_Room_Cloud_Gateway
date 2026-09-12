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
- canonical implementation truth must remain in source and project docs.

## Source-of-truth priority

When information conflicts, use:

1. Current source code and build configuration on the active branch.
2. Current canonical repository docs (`AGENTS.md`, roadmaps, component docs,
   ADRs).
3. Explicit recent build/HIL/manual evidence from Hải.
4. `AI_Stored_Data` handoff notes.
5. Conversation memory or assumptions.

Do not silently resolve a conflict. Record the discrepancy and verify it.

## Update rules

Before significant implementation/review work:

1. Read `AGENTS.md`.
2. Inspect the active branch and relevant current source.
3. Read the relevant roadmap/component docs.
4. Read `PROJECT_STATE.md` and the phase-specific handoff file.
5. Inspect the real worktree before modifying production code.

After a meaningful decision, acceptance result, phase transition, merge, or
architecture/behavior change, update the relevant file here when it materially
helps future sessions.

Use these evidence labels consistently:

- `CONFIRMED` — supported by source, actual build/test logs, or explicit user
  acceptance.
- `PENDING` — not yet validated.
- `DECISION` — an agreed engineering choice.
- `ASSUMPTION` — temporary and not established fact.

Never store credentials, Wi-Fi passwords, PoP values, activation secrets,
transport tokens, private keys, service-account JSON, personal secrets, or
private payloads here.

## Current synchronization snapshot

Latest handoff synchronization: **2026-09-12**.

Active integration branch:

```text
main_including_Firebase_security
```

Observed production/source HEAD before the 2026-09-12 AI metadata synchronization:

```text
15cd0f06d25142a6ed7672bc99dfd4ec396184b0
```

Current major state:

```text
Phase 16 / 16.1  accepted baseline, endurance/integration follow-up remains
Phase 17         COMPLETE, read-only MCP voice HIL accepted
Phase 18         IN PROGRESS
Phase 18.1       COMPLETE; current-source build PASS; target HIL accepted by user
Phase 18.2-18.4  NOT STARTED
Phase 19         NOT STARTED
```

Important current-source updates verified during the Phase-18.1 closure
include dynamic TLS buffers in PSRAM, a 20 KiB PSRAM pre-PTT headroom gate,
0.96-second streaming prefill with the timeout starting after first PCM, and
updated Phase-18.1 effect semantics. Read `PROJECT_STATE.md` before relying on
older phase notes.

A canonical documentation discrepancy is currently recorded: source and this
handoff show Phase 18 in progress, while `XIAOZHI_IMPLEMENTATION_ROADMAP.md`
still contains an older "Sprint 18 — Not Started" status. Do not silently use
that stale status as current implementation truth.

## File index

### Core handoff state

- `PROJECT_STATE.md` — current integrated snapshot, branch/source anchor,
  ownership, recent code changes, pending validation, and next action.
- `DECISIONS.md` — durable project decisions and superseding constraints.
- `NEXT_WORK_AND_HIL_BACKLOG.md` — current validation priority plus historical
  HIL routing/deferred work.
- `COMPONENT_PORTABILITY_HARDENING.md` — merged portability-hardening record and
  frozen architecture conclusions.

### Current Xiaozhi phases

- `PHASE16_PROGRESS.md` — Phase-16 arbitration closure.
- `PHASE16_HIL_TEST_PLAN.md`
- `PHASE16_HIL_EVIDENCE.md`
- `PHASE16_HIL_TEST_BRANCH.md`
- `PHASE16_1_STREAMING_DOWNLINK.md`
- `PHASE17_XIAOZHI_SENSOR_ANSWER.md`
- `PHASE18_MCP_CONTROLLED_ACTIONS.md` — approved Phase-18 scope, exact current
  Phase-18.1 behavior, validation history, and next HIL/rebuild requirements.

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

This directory remains support metadata only and is intentionally safe to
delete without changing firmware behavior.
