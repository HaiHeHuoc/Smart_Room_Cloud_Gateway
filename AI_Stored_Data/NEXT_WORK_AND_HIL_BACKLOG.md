# Next Work + Deferred HIL Backlog

Updated: 2026-08-25
Authoritative development branch: `phase/14-ptt-voice-mvp`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"**

## Routing rule

Always surface the deferred HIL backlog before recommending new coding work. Hardware-unavailable HIL must remain visible but must not block software development.

Current software state:

```text
Phase 12 SW -> COMPLETE / selected HIL deferred
Phase 13 SW -> COMPLETE / HIL deferred
Phase 14 SW -> COMPLETE / Build + HIL pending
```

Do not start Phase 15 automatically. Only recommend/start it after Hải explicitly asks.

---

## Phase 12 HIL — deferred / Codex-ready

Branch:

`test/xiaozhi-p2f-known-audio-e2e`

Primary runbook:

`AI_Stored_Data/CODEX_HIL_RUNBOOK.md`

Activation label:

`RUN PHASE 12 HIL`

Backlog includes:

- P2-F known-audio E2E from SD;
- BOOT `Starting...` regression;
- real AP/Internet/service loss;
- resource/cleanup baseline.

When hardware is unavailable: **DEFERRED**.

---

## Phase 13 HIL — deferred / Codex-ready

Branch:

`test/phase13-voice-assistant-hil`

Primary runbook:

`AI_Stored_Data/CODEX_HIL_RUNBOOK.md`

Activation label:

`RUN PHASE 13 HIL`

Backlog includes:

- repeated production session start/stop;
- duplicate command rejection;
- audio-status coalescing;
- real network-loss recovery;
- stale/late event handling;
- resource trend.

When hardware is unavailable: **DEFERRED**.

---

## Phase 14 HIL — plan ready / test branch not yet created

Production branch:

`phase/14-ptt-voice-mvp`

HIL plan:

`AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`

Recommended future test branch:

`test/phase14-ptt-voice-e2e-hil`

Phase-14 target acceptance must prove:

```text
physical PTT
-> real Xiaozhi READY
-> INMP441 capture
-> PCM uplink
-> server response
-> downlink aggregation
-> audio_manager playback
-> MAX98357 speaker
-> cleanup
-> repeated next turn
```

Important acceptance risks:

- actual downlink codec must be proven compatible with the current PCM16 assumption;
- temporary GPIO5 pull-down/active-high PTT wiring must be verified;
- SD-backed response WAV handoff must work under `sd_card_manager` ownership;
- repeated turns must not race mic/speaker I2S ownership;
- source-scoped CMake composition/tap redirects require real build evidence.

When hardware is unavailable: **DEFERRED / TEST PLAN READY / TEST BRANCH PENDING USER REQUEST**.

---

## What to answer when asked "làm gì tiếp theo?"

### If hardware is NOT available

Answer in this order:

1. Phase 12 HIL remains deferred and Codex-ready.
2. Phase 13 HIL remains deferred and Codex-ready.
3. Phase 14 HIL remains deferred; plan is ready but dedicated test branch has not yet been created.
4. Phase 14 software is complete.
5. Do not start Phase 15 unless Hải explicitly requests continuation.

Concise state:

```text
P12 HIL -> DEFERRED / test branch ready
P13 HIL -> DEFERRED / test branch ready
P14 HIL -> DEFERRED / plan ready
P14 SW  -> COMPLETE
Next SW -> Phase 15 only after explicit request
```

### If hardware IS available

Recommended order unless a specific regression requires otherwise:

1. `RUN PHASE 12 HIL` and close older transport/startup evidence.
2. `RUN PHASE 13 HIL` and close voice-session lifecycle evidence.
3. Create/use the Phase-14 dedicated HIL branch and run full PTT voice E2E.
4. Fix production defects on the owning production branch, propagate forward/test branches, then retest.
5. After Phase 12/13/14 independent acceptance, integrate production commits into the full Gateway/Firebase integration branch and run a full regression HIL.

Never merge HIL/test harness branches as production feature history.

## Evidence discipline

Use these states precisely:

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies the documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
