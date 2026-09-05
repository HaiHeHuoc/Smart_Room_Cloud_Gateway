# AI_Stored_Data

## Purpose

This directory is a shared handoff store for AI-assisted work on the Smart Room Cloud Gateway repository.

It exists to synchronize durable project context between ChatGPT conversations, Codex sessions, and other AI-assisted workflows that can read the repository.

Use it for information that has been reviewed or explicitly agreed with Hải and that is useful across sessions, such as:

- current project/phase state;
- important engineering decisions and constraints;
- accepted hardware/build/test evidence;
- unresolved items and next actions;
- handoff notes needed to resume work safely.

## Authority

Hải explicitly authorizes AI assistants working on this repository to create, overwrite, reorganize, and update files inside `AI_Stored_Data/` as needed to keep the stored context current.

This authorization applies only to this directory. Normal repository rules in `AGENTS.md` still apply everywhere else.

Hải may delete this directory at any time. Therefore:

- production code must never depend on it;
- CMake/Kconfig/build logic must never require it;
- runtime behavior must never read it;
- important implementation truth must still live in source code and canonical project documentation.

## Source-of-truth priority

When information conflicts, use this priority:

1. Current source code and build configuration on the active branch.
2. Current canonical repository documentation (`AGENTS.md`, roadmaps, component docs, ADRs).
3. Explicit recent hardware/build/test evidence from Hải.
4. `AI_Stored_Data` handoff notes.
5. Conversation memory or assumptions.

Do not silently resolve conflicts. Record the discrepancy and verify it.

## Update rules

Before significant implementation/review work:

1. Read `AGENTS.md`.
2. Read the relevant roadmap/component documentation.
3. Inspect the active branch implementation.
4. Read this directory for cross-session handoff context.

After an important decision, acceptance result, phase transition, or architectural change, update the relevant file here when doing so materially helps future sessions.

Keep entries concise and evidence-based. Distinguish clearly between:

- `CONFIRMED` — supported by repository state, build/test logs, or explicit user acceptance;
- `PENDING` — not yet validated;
- `DECISION` — an agreed engineering choice;
- `ASSUMPTION` — temporary and must not be treated as fact.

Never store credentials, activation codes/challenges, transport tokens, Wi-Fi passwords, private payloads, personal secrets, or confidential client information here.

## Initial files

- `PROJECT_STATE.md` — compact current-state snapshot and next-work handoff.
- `DECISIONS.md` — durable project decisions/constraints agreed with Hải.

This directory is support metadata only and is intentionally safe to delete without affecting firmware behavior.
