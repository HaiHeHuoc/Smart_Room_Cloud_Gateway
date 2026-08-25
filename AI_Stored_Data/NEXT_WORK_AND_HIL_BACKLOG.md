# Next Work + HIL Routing

Updated: 2026-08-25
Authoritative development branch: `phase/14-ptt-voice-mvp`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"**

## Routing rule

Use completed HIL as regression baselines and keep the remaining Phase-14 hardware acceptance visible.

Current software state:

```text
Phase 12     -> SOFTWARE COMPLETE / HIL PASS
Phase 13     -> SOFTWARE COMPLETE / HIL PASS
Phase 14 SW  -> COMPLETE / BUILD PASS
Phase 14 HIL -> TEST BRANCH READY / PENDING
```

Do not start Phase 15 automatically. Only recommend/start it after Hải explicitly asks.

---

## Phase 12 HIL — closed

Branch:

`test/xiaozhi-p2f-known-audio-e2e`

Target acceptance: **PASS / closed 2026-08-25**. Retain the branch as the Phase-12 regression baseline.

---

## Phase 13 HIL — closed

Branch:

`test/phase13-voice-assistant-hil`

Target acceptance: **PASS / closed 2026-08-25**. Accepted evidence covers repeated lifecycle, duplicate gates, audio coalescing, real AP loss, explicit recovery, bounded connect failure, generation isolation and resource trend.

---

## Phase 14 HIL — test branch ready

Production branch:

`phase/14-ptt-voice-mvp`

HIL plan:

`AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`

Dedicated test branch:

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
- source-scoped CMake composition/tap redirects have build evidence; runtime ownership still requires HIL.

Current state: **PRODUCTION BUILD PASS / TEST BRANCH READY / TARGET HIL PENDING**.

---

## What to answer when asked "làm gì tiếp theo?"

### If hardware is NOT available

Answer in this order:

1. Phase 12 and Phase 13 HIL are closed with PASS evidence.
2. Phase 14 software and production build are complete.
3. Phase 14 HIL is pending on its dedicated test branch.
4. Do not start Phase 15 unless Hải explicitly requests continuation.

Concise state:

```text
P12 HIL -> PASS / closed
P13 HIL -> PASS / closed
P14 HIL -> PENDING / test branch ready
P14 SW  -> COMPLETE / build PASS
Next SW -> Phase 15 only after explicit request
```

### If hardware IS available

Recommended order unless a specific regression requires otherwise:

1. Use the Phase-14 dedicated HIL branch and run full PTT voice E2E.
2. Fix production defects on the owning production branch, propagate forward/test branches, then retest.
3. After Phase-14 acceptance, integrate production commits into the full Gateway/Firebase integration branch and run a full regression HIL.

Never merge HIL/test harness branches as production feature history.

## Evidence discipline

Use these states precisely:

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies the documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
