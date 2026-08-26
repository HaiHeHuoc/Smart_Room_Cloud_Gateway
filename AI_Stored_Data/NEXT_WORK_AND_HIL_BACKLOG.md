# Next Work + HIL Routing

Updated: 2026-08-26
Authoritative development branch: `phase/14-ptt-voice-mvp`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"**

## Routing rule

Use completed HIL as regression baselines and keep the remaining Phase-14 hardware acceptance visible.

Current software state:

```text
Phase 12     -> SOFTWARE COMPLETE / HIL PASS
Phase 13     -> SOFTWARE COMPLETE / HIL PASS
Phase 14 SW  -> COMPLETE / BUILD PASS
Phase 14 HIL -> GOLDEN-PATH PASS / REPOSITORY CLOSURE IN PROGRESS
```

Do not start Phase 15 implementation automatically. The operator has now
explicitly requested the Phase-14 closure and Phase-15 test preparation.

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

## Phase 14 HIL — golden path accepted

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

Remaining boundaries:

- fault-injection cases (stalled response, network loss, SD unavailable and
  queue pressure) were not injected and remain SKIP/deferred;
- GPIO38 pull-down/active-high wiring was verified on target; GPIO48 remains
  reserved for NeoPixel;
- the LCD `Starting...` route remains a Phase-15 UI concern;
- source-scoped CMake composition/tap redirects are build- and runtime-tested
  for the accepted voice path.

Current state: **BUILD PASS / FLASH PASS / GOLDEN-PATH HIL PASS / FAULT CASES SKIP**.

The corrected target run used GPIO38 and completed three consecutive PTT
turns: Opus uplink, server response, PCM16 decode, SD-backed WAV playback and
`PLAYBACK_COMPLETE`. The operator confirmed audible speaker output. Two
release-before-READY attempts also completed bounded cancellation. The
stalled-response, network-loss, SD-unavailable and queue-pressure cases were
not injected and remain explicitly deferred. The LCD `Starting...` route is a
separate Phase-15 UI concern.

---

## What to answer when asked "làm gì tiếp theo?"

### If hardware is NOT available

Answer in this order:

1. Phase 12 and Phase 13 HIL are closed with PASS evidence.
2. Phase 14 software and production build are complete.
3. Phase 14 golden-path HIL is PASS; fault cases remain SKIP/deferred.
4. Complete the Phase-14 documentation/Git checkpoint before Phase 15 HIL.

Concise state:

```text
P12 HIL -> PASS / closed
P13 HIL -> PASS / closed
P14 HIL -> GOLDEN-PATH PASS / fault cases deferred
P14 SW  -> COMPLETE / build PASS
Next SW -> Phase 15 HIL/UI checkpoint after Phase-14 closure
```

### If hardware IS available

Recommended order unless a specific regression requires otherwise:

1. Close the Phase-14 documentation and Git checkpoint.
2. Propagate the Phase-14 production commit into the Phase-15 production branch.
3. Merge the Phase-15 production branch into its dedicated test branch and run
   `RUN PHASE 15 HIL`.

Never merge HIL/test harness branches as production feature history.

## Evidence discipline

Use these states precisely:

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies the documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
