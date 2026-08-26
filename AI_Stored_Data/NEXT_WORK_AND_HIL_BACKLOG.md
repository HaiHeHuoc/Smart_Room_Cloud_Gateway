# Next Work + Deferred HIL Backlog

Updated: 2026-08-26
Authoritative development branch: `phase/15-voice-assistant-ui`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"**

## Routing rule

Use completed HIL as regression baselines and keep the remaining Phase-14
fault cases plus Phase-15 UI HIL visible before recommending new coding work.

Current software state:

```text
Phase 12 SW -> COMPLETE / HIL PASS
Phase 13 SW -> COMPLETE / HIL PASS
Phase 14 SW -> COMPLETE / BUILD PASS
Phase 14 HIL -> GOLDEN-PATH PASS / fault cases deferred
Phase 15 SW -> COMPLETE / Build + HIL pending
Phase 16    -> NOT STARTED
```

Do not start Phase 16 automatically. Only start it after explicit user direction.

---

## Phase 12 HIL — closed / regression baseline

Branch: `test/xiaozhi-p2f-known-audio-e2e`

Primary runbook: `AI_Stored_Data/CODEX_HIL_RUNBOOK.md`

Activation label: `RUN PHASE 12 HIL`

Backlog includes:

- P2-F known-audio E2E;
- BOOT `Starting...` regression;
- real AP/Internet/service loss;
- resource/cleanup baseline.

Target acceptance is already closed; rerun only as an explicit regression.

---

## Phase 13 HIL — closed / regression baseline

Branch: `test/phase13-voice-assistant-hil`

Primary runbook: `AI_Stored_Data/CODEX_HIL_RUNBOOK.md`

Activation label: `RUN PHASE 13 HIL`

Backlog includes:

- repeated production session start/stop;
- duplicate command rejection;
- audio-status coalescing;
- real network-loss recovery;
- stale/late event handling;
- resource trend.

Target acceptance is already closed; rerun only as an explicit regression.

---

## Phase 14 HIL — golden path accepted / fault cases deferred

Production branch: `phase/14-ptt-voice-mvp`

HIL plan: `AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`

Recommended test branch: `test/phase14-ptt-voice-e2e-hil`

Acceptance recorded on 2026-08-26: three GPIO38 PTT turns reached real
Xiaozhi response playback and were audible. Opus uplink/decode, WAV
completion, repeated turns and release-before-READY evidence passed. Fault
injection cases remain SKIP/deferred; the LCD `Starting...` route is tracked
by Phase 15 UI HIL.

Target acceptance:

```text
physical PTT
-> real Xiaozhi READY
-> INMP441 capture
-> PCM uplink
-> server response
-> downlink
-> audio_manager
-> MAX98357 speaker
-> cleanup
-> repeated next turn
```

Important risks:

- deferred fault-injection coverage;
- GPIO38 PTT wiring (GPIO48 reserved for NeoPixel);
- SD-backed response handoff;
- repeated-turn audio ownership;
- source-scoped CMake integration/tap build compatibility.

When hardware is unavailable for a new rerun: preserve the recorded
golden-path PASS and mark only unexecuted fault cases **DEFERRED**.

---

## Phase 15 HIL — deferred / plan ready

Production branch: `phase/15-voice-assistant-ui`

Progress/closure: `AI_Stored_Data/PHASE15_PROGRESS.md`

HIL plan: `AI_Stored_Data/PHASE15_HIL_TEST_PLAN.md`

Recommended test branch: `test/phase15-voice-ui-hil`

Suggested activation label: `RUN PHASE 15 HIL`

Target acceptance:

```text
real voice lifecycle
-> Voice/Xiaozhi screen
-> real USER CHAT_TEXT
-> copied USER transcript on LCD
-> real ASSISTANT CHAT_TEXT
-> copied ASSISTANT text on LCD
-> assistant text independent from audio playback
-> repeated latest-turn replacement
-> session-change stale-text cleanup
-> ERROR/RECOVERING presentation
-> UI/LVGL ownership remains stable
```

Important Phase-15 risks:

- source-local `esp_xiaozhi_chat_init` semantic bridge must compile against pinned `esp_xiaozhi` 0.1.2;
- USER/ASSISTANT semantic pointer lifetime must be proven by real callback behavior;
- recovery snapshot must remain accepted by legacy `app_gui` after the final-review error-normalization fix;
- legacy screen intentionally maps CONNECTING/THINKING/RECOVERING to `PROCESSING`;
- Phase-14 audio/codec failures must not be misdiagnosed as Phase-15 text/UI failures.

When hardware is unavailable: **DEFERRED / TEST PLAN READY**.

---

## Post-Phase-15 architecture follow-up — audio arbitration

Before intentionally enabling multiple competing audio clients, add/review centralized `audio_manager` capture/playback arbitration.

Examples:

```text
Xiaozhi SPEAKING + notification playback
Xiaozhi LISTENING + another recorder
critical alarm + Xiaozhi response playback
```

The current `audio_manager` sole-I2S ownership is correct, but general request priority/queue/preemption policy is not yet implemented.

Do not solve future contention by giving another component direct I2S ownership.

---

## What to answer when asked "làm gì tiếp theo?"

### If hardware is NOT available

Answer in this order:

1. Phase 12 HIL is closed and retained as a regression baseline.
2. Phase 13 HIL is closed and retained as a regression baseline.
3. Phase 14 golden-path HIL is PASS; fault cases remain deferred.
4. Phase 15 HIL remains deferred; plan/test branch ready.
5. Phase 14 and Phase 15 software are complete.
6. General audio arbitration remains a future integration/architecture follow-up.
7. Phase 16 is not started; only plan/start it after explicit request.

Concise state:

```text
P12 HIL -> PASS / closed baseline
P13 HIL -> PASS / closed baseline
P14 HIL -> GOLDEN-PATH PASS / fault cases deferred
P15 HIL -> DEFERRED / test branch ready
P14 SW  -> COMPLETE / BUILD PASS
P15 SW  -> COMPLETE / build/HIL pending
P16     -> NOT STARTED
```

### If hardware IS available

Recommended order unless a specific regression requires otherwise:

1. Finish publishing the Phase-14 production and HIL checkpoints.
2. Create/use the Phase-15 dedicated HIL branch and run real semantic
   text/UI acceptance.
3. Fix production defects on the owning phase branch and propagate
   forward/test branches.
4. After independent acceptance, integrate **production branches/history
   only** into the full Gateway/Firebase integration branch.
5. Run full regression HIL: Wi-Fi/provisioning + sensor + Firebase + GUI +
   SD + audio + Xiaozhi + Voice UI, including simultaneous Firebase/Xiaozhi
   load.

Never merge HIL/test harness branches as production feature history.

## Production-vs-test fix policy

```text
Test harness/config/expected-log defect
-> fix on test branch

Production component/architecture defect
-> fix on owning production branch
-> propagate forward/test branch
-> retest
```

Preserve evidence before patching a failed HIL case.

## Evidence discipline

Use these states precisely:

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
