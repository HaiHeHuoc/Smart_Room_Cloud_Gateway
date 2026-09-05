# Phase 13 Roadmap Reconciliation

Updated: 2026-08-25
Branch: `phase/13-voice-assistant`
Status: **AUTHORITATIVE HANDOFF FOR CURRENT PHASE-13 BOUNDARY**

## Why this note exists

`XIAOZHI_IMPLEMENTATION_ROADMAP.md` was written before the Phase-12/13 implementation clarified several ownership boundaries. Its Sprint-13 section still groups some Push-to-Talk, live audio and presentation responsibilities inside the adapter phase.

The implemented architecture intentionally keeps those responsibilities in the later sprints where they can be owned correctly.

Until the canonical roadmap receives a dedicated cleanup pass, use this note together with `AI_Stored_Data/PHASE13_PROGRESS.md` and `AI_Stored_Data/PROJECT_STATE.md` when deciding whether Phase 13 is complete or what Codex should implement next.

## Current authoritative split

### Sprint 13 — voice_assistant foundation

Owned here:

- project-owned conversation state machine;
- bounded orchestration task/queue;
- session generation and stale-event filtering;
- production Xiaozhi WebSocket session lifecycle through `xiaozhi_foundation`;
- transport READY/error/goodbye/stop semantics;
- explicit bounded recovery policy;
- copied audio-manager status contract;
- UI-safe scalar voice status contract;
- security/ownership boundary;
- validation-vs-production isolation.

Status: **SOFTWARE COMPLETE / HIL PENDING**.

### Sprint 14 — Push-To-Talk Voice MVP

Owned here, not pulled backward into Sprint 13:

- user-authorized PTT/cancel trigger;
- actual `voice_assistant` runtime start/session trigger in application composition;
- live microphone PCM export contract from `audio_manager`;
- Opus/audio-channel uplink;
- LISTENING / THINKING / SPEAKING transaction semantics;
- response-audio buffering/playback through `audio_manager`;
- network-loss/cancel behavior during an active turn;
- bounded real conversation lifecycle.

### Sprint 15 — GUI Voice / Chatbot Presentation

Owned here:

- transcript presentation;
- emotion/animation presentation;
- final GUI voice queue/model;
- UTF-8-aware bounded text handling;
- LVGL voice rendering and animation priority.

## Direct dependency clarification

The old Sprint-13 acceptance line says only `voice_assistant` may directly depend on `esp_xiaozhi`.

The implemented architecture deliberately preserves the Phase-12 project-owned boundary:

```text
voice_assistant
    -> xiaozhi_foundation public API
        -> esp_xiaozhi / MCP
```

Therefore the authoritative ownership rule is now:

> `xiaozhi_foundation` is the sole direct `esp_xiaozhi`/MCP dependency boundary. `voice_assistant` must not include or expose `esp_xiaozhi` types.

This is stricter layering than making the application orchestrator depend directly on the managed component and preserves the audited Phase-12 lifecycle/security boundary.

## What not to do

Do not reopen Phase 13 merely because the old roadmap text still mentions:

- PTT APIs;
- live audio uplink/downlink;
- transcript/emotion GUI payloads;
- automatic network-driven conversation behavior.

Those tasks are intentionally assigned to Sprint 14/15 above.

Do reopen Phase 13 only if build/static/HIL evidence finds a defect in the implemented Phase-13 foundation itself.

## Next software task when hardware is unavailable

Start Sprint 14 planning/implementation from the current Phase-13 branch state, after re-reading:

1. `AGENTS.md`;
2. `AI_Stored_Data/PROJECT_STATE.md`;
3. `AI_Stored_Data/PHASE13_PROGRESS.md`;
4. this reconciliation note;
5. current public `audio_manager`, `voice_assistant`, and `xiaozhi_foundation` headers.

Do not block Sprint-14 software work on the deferred Phase-12/13 HIL backlog.
