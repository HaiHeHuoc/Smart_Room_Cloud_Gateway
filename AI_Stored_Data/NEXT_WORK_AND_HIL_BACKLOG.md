# Next Work + Deferred HIL Backlog

Updated: 2026-08-25
Authoritative development branch: `phase/16-audio-arbitration`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"**

## Routing rule

Always surface the deferred HIL backlog before recommending new coding work. Hardware-unavailable HIL remains visible but does not block software development.

Current software state:

```text
Phase 12 SW -> COMPLETE / selected HIL deferred
Phase 13 SW -> COMPLETE / HIL deferred
Phase 14 SW -> COMPLETE / Build + HIL pending
Phase 15 SW -> COMPLETE / Build + HIL pending
Phase 16 SW -> IN PROGRESS / 16-A complete
```

Phase 16 was explicitly started from `phase/15-voice-assistant-ui`. Continue one checkpoint at a time and only after Hải says `tiếp tục`.

---

## Phase 12 HIL — deferred / Codex-ready

Branch: `test/xiaozhi-p2f-known-audio-e2e`
Activation label: `RUN PHASE 12 HIL`

Backlog includes P2-F known-audio E2E, BOOT `Starting...` regression, real AP/Internet/service loss and resource/cleanup evidence.

When hardware is unavailable: **DEFERRED**.

---

## Phase 13 HIL — deferred / Codex-ready

Branch: `test/phase13-voice-assistant-hil`
Activation label: `RUN PHASE 13 HIL`

Backlog includes repeated production session lifecycle, duplicate-command rejection, audio-status coalescing, network-loss recovery, stale/late events and resource trend.

When hardware is unavailable: **DEFERRED**.

---

## Phase 14 HIL — deferred / plan ready

Production branch: `phase/14-ptt-voice-mvp`
HIL plan: `AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`
Recommended test branch: `test/phase14-ptt-voice-e2e-hil`

Target acceptance remains physical PTT -> Xiaozhi READY -> INMP441 -> uplink -> response -> audio_manager -> MAX98357 -> repeated turn.

When hardware is unavailable: **DEFERRED / TEST PLAN READY**.

---

## Phase 15 HIL — deferred / test branch ready

Production branch: `phase/15-voice-assistant-ui`
Test branch: `test/phase15-voice-ui-hil`
HIL plan: `AI_Stored_Data/PHASE15_HIL_TEST_PLAN.md`
Activation label: `RUN PHASE 15 HIL`

Target acceptance includes real USER/ASSISTANT semantic text, copied LCD presentation, repeated latest-turn behavior, stale-generation cleanup, ERROR/RECOVERING presentation and LVGL ownership stability.

When hardware is unavailable: **DEFERRED / TEST BRANCH READY**.

---

## Phase 16 — active software work

Production branch: `phase/16-audio-arbitration`
Progress: `AI_Stored_Data/PHASE16_PROGRESS.md`

Goal: add centralized multi-client capture/playback arbitration without giving any client direct I2S ownership.

Current checkpoint:

```text
16-A Audio client/request model                 COMPLETE
16-B Playback arbitration runtime               NEXT
16-C Capture arbitration runtime                pending
16-D Priority/preemption/queue hardening        pending
16-E Xiaozhi + notification/alarm integration   pending
16-F Final review/HIL plan                      pending
```

16-A added project-owned metadata for logical clients, CAPTURE/PLAYBACK resources, priority, busy policy and interruptibility. Existing production recording/playback APIs intentionally keep their Phase-15 behavior until later checkpoints explicitly migrate them behind arbitration.

Examples Phase 16 will resolve:

```text
Xiaozhi SPEAKING + notification playback
Xiaozhi SPEAKING + critical alarm
Xiaozhi LISTENING + another recorder request
```

Important rule:

```text
FreeRTOS task concurrency              allowed
multiple direct I2S owners             forbidden
multiple audio_manager client requests arbitrated centrally
```

Do not solve contention by pausing unrelated sensor/Firebase tasks or by giving another component an I2S handle.

---

## What to answer when asked "làm gì tiếp theo?"

### If hardware is NOT available

Answer in this order:

1. Phase 12 HIL deferred / Codex-ready.
2. Phase 13 HIL deferred / Codex-ready.
3. Phase 14 HIL deferred / plan ready.
4. Phase 15 HIL deferred / test branch ready.
5. Phase 16 software is active; 16-A is complete.
6. Next software checkpoint is **16-B — Playback Arbitration Runtime**, but only implement after explicit `tiếp tục`.

Concise state:

```text
P12 HIL -> DEFERRED
P13 HIL -> DEFERRED
P14 HIL -> DEFERRED
P15 HIL -> DEFERRED / test branch ready
P16 SW  -> 16-A COMPLETE
Next SW -> 16-B after `tiếp tục`
```

### If hardware IS available

Recommended acceptance order remains:

1. Phase 12 HIL.
2. Phase 13 HIL.
3. Phase 14 PTT voice E2E HIL.
4. Phase 15 voice UI HIL.
5. Fix production defects on their owning production branch and propagate forward.
6. After independent acceptance, integrate production history into the full Gateway/Firebase integration branch.
7. Run full regression with Wi-Fi/provisioning + sensor + Firebase + GUI + SD + audio + Xiaozhi, including simultaneous cloud/voice load and Phase-16 multi-client audio cases once implemented.

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

## Evidence discipline

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
