# Next Work + Deferred HIL Backlog

Updated: 2026-08-25
Authoritative development branch: `phase/16-audio-arbitration`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"** and HIL activation from any clean working branch.

## Current software state

```text
Phase 12 SW -> COMPLETE / selected HIL deferred
Phase 13 SW -> COMPLETE / HIL deferred
Phase 14 SW -> COMPLETE / Build + HIL pending
Phase 15 SW -> COMPLETE / Build + HIL pending
Phase 16 SW -> COMPLETE / Static review complete / Build + HIL pending
Major feature coding -> COMPLETE through Phase 16
```

Do not start Phase 17 automatically. The next project stage is acceptance/integration/hardening unless Hải explicitly expands feature scope.

## Global Codex HIL routing

Commands may be entered from the latest branch, but Codex must route to the dedicated test branch before testing:

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL -> test/phase16-audio-arbitration-hil
```

Before checkout, inspect git status. If local work could be lost, stop and report; never auto-stash/reset/delete. After routing, Codex must read the HIL plan/runbook on the selected test branch before editing/building/flashing.

## Deferred HIL

- Phase 12: `test/xiaozhi-p2f-known-audio-e2e`, activation `RUN PHASE 12 HIL`.
- Phase 13: `test/phase13-voice-assistant-hil`, activation `RUN PHASE 13 HIL`.
- Phase 14: plan ready; dedicated `test/phase14-ptt-voice-e2e-hil` prerequisite must still be verified/created before execution.
- Phase 15: `test/phase15-voice-ui-hil`, activation `RUN PHASE 15 HIL`.
- Phase 16: `test/phase16-audio-arbitration-hil`, plan `AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md`, runbook `AI_Stored_Data/PHASE16_HIL_TEST_BRANCH.md`, activation `RUN PHASE 16 HIL`.

All remain DEFERRED when hardware is unavailable.

## Phase 16 closure

Production branch: `phase/16-audio-arbitration`
Closure: `AI_Stored_Data/PHASE16_PROGRESS.md`
Test branch: `test/phase16-audio-arbitration-hil`
Activation: `RUN PHASE 16 HIL`

Completed checkpoints:

```text
16-A Audio client/request model                 COMPLETE
16-B Playback arbitration runtime               COMPLETE
16-C Capture arbitration runtime                COMPLETE
16-D Priority/preemption/queue hardening        COMPLETE
16-E Xiaozhi + notification/alarm integration   COMPLETE
16-F Final review/HIL plan                      COMPLETE
```

Final policy summary:

```text
XIAOZHI capture  priority 70  REJECT  interruptible
XIAOZHI playback priority 70  QUEUE   interruptible
NOTIFICATION     priority 50  QUEUE   interruptible
ALARM            priority 100 PREEMPT_LOWER_PRIORITY non-interruptible
```

`audio_manager` remains the sole I2S/DMA owner. Capture and playback arbiters each keep one current + one pending request. Equal/lower priority never preempts. Unknown external/legacy activity is never preempted. Global cross-resource fairness is not claimed.

Known accepted technical debt entering HIL:

- no target build evidence yet;
- no HIL evidence yet;
- arbiter stop/deinit lifecycle APIs absent;
- source-local CMake bridges need build proof;
- recorded-audio playback not migrated;
- Xiaozhi downlink still waits for manager IDLE before playback submit;
- global capture/playback fairness not guaranteed;
- Phase-14 codec/SD-backed playback risks remain.

## What to answer when asked "làm gì tiếp theo?"

If hardware is unavailable:

1. Phase 12 HIL deferred / Codex-ready.
2. Phase 13 HIL deferred / Codex-ready.
3. Phase 14 HIL deferred / dedicated test-branch prerequisite.
4. Phase 15 HIL deferred / test branch ready.
5. Phase 16 HIL deferred / **test branch + Codex runbook ready**.
6. Major feature coding is complete. Do not propose Phase 17 automatically.
7. Useful non-HIL work is limited to documentation/review/test-harness preparation; avoid new feature scope without explicit user direction.

If hardware is available, recommended order:

```text
Phase 12 HIL
-> Phase 13 HIL
-> Phase 14 HIL
-> Phase 15 HIL
-> Phase 16 HIL
-> fix production defects on owning phase branch and propagate forward
-> integrate production history into full Gateway/Firebase branch
-> full system regression
-> performance/resource/stability hardening
-> final documentation/release/portfolio closure
```

Full regression must include Wi-Fi/provisioning + sensor + Firebase + GUI + SD + audio + Xiaozhi, simultaneous Firebase/Xiaozhi traffic, repeated PTT turns, notification queueing and critical-alarm preemption.

## `RUN PHASE 16 HIL` Codex behavior

From any clean branch, including `phase/16-audio-arbitration`, Codex must:

```text
inspect branch + git status
-> checkout test/phase16-audio-arbitration-hil
-> read Agent.md + Phase-16 HIL plan + Phase-16 HIL branch runbook
-> inspect current test/production seams
-> clean/reconfigure build
-> flash connected ESP32-S3
-> monitor from reset
-> execute T16-01 ... T16-12 independently
-> request only genuinely manual physical actions
-> report PASS / FAIL / SKIP with runtime evidence
```

Never mark HIL PASS from source inspection. Test-only instrumentation may be added on the test branch. Production defects must be fixed on the owning production branch first and then propagated back for retest.

## Production-vs-test fix policy

```text
Test harness/config/expected-log defect
-> fix on test branch

Production component/architecture defect
-> fix on owning production branch first
-> propagate forward/test branch
-> retest
```

Never merge HIL/test harness history as production feature history.

## Evidence discipline

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
