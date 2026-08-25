# Next Work + Deferred HIL Backlog

Updated: 2026-08-25
Authoritative development branch: `phase/15-voice-assistant-ui`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"**

## Routing rule

Always surface the deferred HIL backlog before recommending new coding work. Hardware-unavailable HIL must remain visible but must not block software development.

Current software state:

```text
Phase 12 SW -> COMPLETE / selected HIL deferred
Phase 13 SW -> COMPLETE / HIL deferred
Phase 14 SW -> COMPLETE / Build + HIL pending
Phase 15 SW -> IN PROGRESS / 15-A complete
```

Phase 15 was explicitly started by Hải from `phase/14-ptt-voice-mvp`. Continue Phase 15 only one checkpoint at a time and only after Hải says `tiếp tục`.

---

## Phase 12 HIL — deferred / Codex-ready

Branch: `test/xiaozhi-p2f-known-audio-e2e`

Primary runbook: `AI_Stored_Data/CODEX_HIL_RUNBOOK.md`

Activation label: `RUN PHASE 12 HIL`

Backlog includes P2-F known-audio E2E, BOOT `Starting...` regression, real AP/Internet/service loss and resource/cleanup baseline.

When hardware is unavailable: **DEFERRED**.

---

## Phase 13 HIL — deferred / Codex-ready

Branch: `test/phase13-voice-assistant-hil`

Primary runbook: `AI_Stored_Data/CODEX_HIL_RUNBOOK.md`

Activation label: `RUN PHASE 13 HIL`

Backlog includes repeated production session start/stop, duplicate command rejection, audio-status coalescing, real network-loss recovery, stale/late event handling and resource trend.

When hardware is unavailable: **DEFERRED**.

---

## Phase 14 HIL — plan ready / test branch not yet created

Production branch: `phase/14-ptt-voice-mvp`

HIL plan: `AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`

Recommended future test branch: `test/phase14-ptt-voice-e2e-hil`

Phase-14 target acceptance must prove physical PTT -> real Xiaozhi READY -> INMP441 capture -> PCM uplink -> server response -> downlink -> audio_manager -> MAX98357 speaker -> cleanup -> repeated next turn.

Important risks remain actual downlink codec, temporary GPIO5 wiring, SD-backed response handoff, repeated-turn ownership and source-scoped CMake integration.

When hardware is unavailable: **DEFERRED / TEST PLAN READY**.

---

## Phase 15 — active software work

Branch: `phase/15-voice-assistant-ui`

Progress document: `AI_Stored_Data/PHASE15_PROGRESS.md`

Current checkpoint:

```text
15-A Production Voice Event/UI Model     COMPLETE
15-B Voice Assistant Screen              NEXT
15-C User transcript                     pending
15-D Assistant response text             pending
15-E Repeated-turn/history/error UX      pending
15-F Final review/HIL plan               pending
```

15-A created a project-owned production presentation model with copied lifecycle/error/generation state and bounded USER/ASSISTANT text buffers. It deliberately does not reuse the temporary Phase-12 validation UI type.

`esp_xiaozhi` exposes semantic `CHAT_TEXT` events with USER/ASSISTANT roles, but production text wiring is intentionally deferred to 15-C/15-D. No transcript/text GUI success is claimed yet.

---

## What to answer when asked "làm gì tiếp theo?"

### If hardware is NOT available

Answer in this order:

1. Phase 12 HIL remains deferred and Codex-ready.
2. Phase 13 HIL remains deferred and Codex-ready.
3. Phase 14 HIL remains deferred; plan is ready.
4. Phase 15 is active; 15-A is complete.
5. Next software checkpoint is **15-B — Voice Assistant Screen**, but do not implement it until Hải says `tiếp tục`.

Concise state:

```text
P12 HIL -> DEFERRED / test branch ready
P13 HIL -> DEFERRED / test branch ready
P14 HIL -> DEFERRED / plan ready
P15 SW  -> 15-A COMPLETE
Next SW -> 15-B after explicit `tiếp tục`
```

### If hardware IS available

Recommended order unless a specific regression requires otherwise:

1. `RUN PHASE 12 HIL`.
2. `RUN PHASE 13 HIL`.
3. Create/use Phase-14 dedicated HIL branch and run PTT voice E2E.
4. Continue/verify Phase-15 UI separately; do not use UI behavior to hide a Phase-14 voice transport/audio failure.
5. Fix production defects on the owning phase branch and propagate forward.
6. After independent acceptance, integrate production commits into the full Gateway/Firebase integration branch and run full regression HIL.

Never merge HIL/test harness branches as production feature history.

## Evidence discipline

Use these states precisely:

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies the documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
