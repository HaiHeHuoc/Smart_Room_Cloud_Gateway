# Phase 16 HIL Test Plan — Audio Arbitration

Updated: 2026-08-25
Production branch: `phase/16-audio-arbitration`
Recommended test branch: `test/phase16-audio-arbitration-hil`
Activation label: `RUN PHASE 16 HIL`
Status: **PLAN READY / BUILD + HIL PENDING**

## Purpose

Prove that Phase-16 multi-client audio arbitration works on the real ESP32-S3 while preserving the Phase-14/15 Xiaozhi voice path and all existing ownership boundaries.

## Acceptance principles

- `audio_manager` remains the sole I2S RX/TX/DMA owner.
- No client receives raw I2S/DMA/file ownership.
- Capture/playback requests are bounded: one current + one pending per arbiter.
- Equal/lower priority never preempts.
- Strictly higher priority may cooperatively preempt only when the known current owner is interruptible.
- Unknown legacy/external activity is never preempted.
- Sensor/Firebase/UI continue to run; Phase 16 does not globally pause unrelated tasks.

## T16-01 — clean build/link

Acceptance:
- arbitration model/policy compiles;
- playback/capture arbiters compile/link;
- named notification/alarm playback helpers compile/link;
- source-local Xiaozhi arbitration bridge compiles without prototype/macro mismatch;
- no duplicate/undefined symbol;
- no warning promoted to error.

## T16-02 — boot/startup regression

Expected startup order:

```text
audio_manager READY
Phase-16 playback arbiter READY
Phase-16 capture arbiter READY
Phase-15 voice stack READY
```

Acceptance:
- boot leaves Starting screen;
- no panic/assert/WDT;
- no duplicate I2S owner;
- no voice-stack startup regression.

## T16-03 — Xiaozhi capture ownership

Start a real PTT turn.

Acceptance:
- Xiaozhi CAPTURE request appears with client=XIAOZHI, priority=70;
- capture arbiter reaches STARTING then ACTIVE only after manager RECORDING evidence;
- mic PCM uplink behavior remains correct;
- PTT release cancels/stops the matching request cooperatively;
- no second recorder/I2S owner is created.

## T16-04 — Xiaozhi playback ownership

Complete a real Xiaozhi response.

Acceptance:
- Xiaozhi PLAYBACK request appears with client=XIAOZHI, priority=70, interruptible=true;
- playback arbiter reaches STARTING then ACTIVE only after manager PLAYBACK evidence;
- response audio still reaches MAX98357;
- completion is recognized only after real PLAYBACK -> IDLE.

Note: the current Phase-14 downlink waits for audio-manager IDLE before submitting Xiaozhi playback, so it does not yet exploit queue-behind-current-playback at this call site. This is accepted for the MVP and must not be mistaken for broken arbitration.

## T16-05 — notification while Xiaozhi speaks

Submit a notification WAV while Xiaozhi playback is ACTIVE.

Acceptance:
- notification client priority=50;
- notification does not preempt Xiaozhi priority=70;
- it is accepted only if the bounded pending slot is available;
- otherwise it is rejected cleanly;
- no corruption/mixed I2S writes occur.

## T16-06 — critical alarm while Xiaozhi speaks

Submit a critical alarm WAV while Xiaozhi playback is ACTIVE.

Acceptance:
- alarm client priority=100, non-interruptible;
- Xiaozhi current owner is interruptible and lower priority;
- arbiter requests cooperative stop via `audio_manager_stop_playback()`;
- manager returns IDLE;
- alarm is promoted and plays;
- no direct I2S manipulation by alarm code.

## T16-07 — equal-priority protection

Submit another playback request with priority equal to current owner under PREEMPT policy.

Acceptance:
- equal priority does not preempt;
- no ping-pong/repeated cancellation loop.

## T16-08 — bounded queue pressure

Fill current + pending slots and submit a third request.

Acceptance:
- third request is rejected deterministically;
- no dynamic/unbounded allocation or queue growth;
- accepted/rejected/queued counters match behavior.

## T16-09 — capture contention

While Xiaozhi is LISTENING, submit a second capture request.

Acceptance:
- Xiaozhi capture uses REJECT policy for its own PTT start and never begins late after speech is over;
- lower/equal priority requester cannot preempt Xiaozhi;
- strictly higher-priority known interruptible policy follows documented behavior if an appropriate test client is used;
- manager remains the sole I2S RX owner.

## T16-10 — simultaneous capture/playback request race

Create near-simultaneous CAPTURE and PLAYBACK requests while manager is IDLE.

Acceptance:
- only one manager command wins hardware serialization;
- the loser observes busy/invalid-state and retries/waits according to its arbiter;
- no duplicate active operation;
- no crash/deadlock.

Global cross-resource fairness is not claimed by Phase 16. If sustained test proves starvation, record evidence for a future unified audio scheduler rather than hiding it with an unbounded queue.

## T16-11 — cancellation edge cases

Exercise:
- cancel before actual RECORDING/PLAYBACK starts;
- cancel current ACTIVE request;
- cancel pending request.

Acceptance:
- pre-start cancel removes logical ownership without unnecessary hardware stop;
- active cancel is cooperative through audio_manager;
- pending cancel removes only matching pending request;
- no stuck current/pending slot.

## T16-12 — full Gateway coexistence

Run sensor, Firebase/cloud, GUI, SD, Xiaozhi and arbitration scenarios together.

Acceptance:
- unrelated tasks continue progress;
- no global Firebase/sensor pause is introduced;
- no panic/assert/WDT;
- audio queue/drop/resource counters remain understandable;
- no monotonic heap/task/handle leak across repeated turns and notification/alarm requests.

## Evidence required

For each case preserve:
- serial log excerpt;
- arbiter current/pending metadata;
- accepted/rejected/queued/preemption/completed/failed counters;
- audio_manager state transitions;
- visible/audio behavior where applicable;
- PASS / FAIL / SKIP classification.

## Known accepted software limitations entering HIL

1. playback/capture arbiters currently have init/start but no dedicated stop/deinit API;
2. recorded-audio playback is not migrated into arbitration;
3. capture/playback arbiters are separate and do not guarantee global cross-resource fairness;
4. Phase-14 Xiaozhi downlink waits for manager IDLE before submitting playback, so queue-behind-current-playback is not exercised there;
5. source-local CMake bridge remains a maintainability seam and needs real build evidence;
6. Phase-14 codec/SD-backed playback limitations remain outside Phase-16 arbitration ownership.

No Phase-16 HIL PASS is claimed until target evidence satisfies this plan.
