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

All Phase 12-16 HIL commands may be entered from the latest clean branch. Codex must route to the dedicated test branch before testing:

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL -> test/phase16-audio-arbitration-hil
```

Before checkout, inspect git status. If local work could be lost, stop and report; never auto-stash/reset/delete. After routing, read the phase HIL plan/runbook before editing/building/flashing.

## Synchronized HIL status

```text
P12 test branch -> READY / expected logs documented
P13 test branch -> READY / automatic supervisor + expected PASS/FAIL logs documented
P14 test branch -> CREATED / standardized PH14_TEST markers + expected E2E log story
P15 test branch -> READY / standardized PH15_TEST markers + expected UI/text/audio log story
P16 test branch -> READY / standardized PH16_TEST markers + expected arbitration log story
```

All phases use PASS / FAIL / SKIP evidence discipline. Expected logs are contracts, not observed hardware evidence.

### Phase 12

Branch: `test/xiaozhi-p2f-known-audio-e2e`
Activation: `RUN PHASE 12 HIL`
Primary docs: `PHASE12_HIL_TEST_PLAN.md`, `P2F_KNOWN_AUDIO_HIL.md`, `CODEX_HIL_RUNBOOK.md`.
Expected evidence includes BOOT transition markers, P2-F fixture READY, audio channel open, frames sent, USER/ASSISTANT CHAT_TEXT and validation summary PASS.

### Phase 13

Branch: `test/phase13-voice-assistant-hil`
Activation: `RUN PHASE 13 HIL`
Primary doc: `PHASE13_HIL_TEST_BRANCH.md`.
Expected evidence includes `PH13_HIL` steady-state, audio burst, repeated lifecycle cycles, duplicate guards, optional manual network recovery and final summary.

### Phase 14

Branch: `test/phase14-ptt-voice-e2e-hil`
Activation: `RUN PHASE 14 HIL`
Primary docs: `PHASE14_HIL_TEST_PLAN.md`, `PHASE14_HIL_TEST_BRANCH.md`.
Standard markers:

```text
PH14_TEST T14_XX BEGIN
PH14_TEST T14_XX PASS
PH14_TEST T14_XX FAIL reason=...
PH14_TEST T14_XX SKIP reason=...
PH14_TEST SUMMARY pass=<n> fail=<n> skip=<n>
```

### Phase 15

Branch: `test/phase15-voice-ui-hil`
Activation: `RUN PHASE 15 HIL`
Primary docs: `PHASE15_HIL_TEST_PLAN.md`, `PHASE15_HIL_TEST_BRANCH.md`.
Standard markers:

```text
PH15_TEST T15_XX BEGIN
PH15_TEST T15_XX PASS
PH15_TEST T15_XX FAIL reason=...
PH15_TEST T15_XX SKIP reason=...
PH15_TEST SUMMARY pass=<n> fail=<n> skip=<n>
```

### Phase 16

Branch: `test/phase16-audio-arbitration-hil`
Activation: `RUN PHASE 16 HIL`
Primary docs: `PHASE16_HIL_TEST_PLAN.md`, `PHASE16_HIL_TEST_BRANCH.md`.
Standard markers:

```text
PH16_TEST T16_XX BEGIN
PH16_TEST T16_XX PASS
PH16_TEST T16_XX FAIL reason=...
PH16_TEST T16_XX SKIP reason=...
PH16_TEST SUMMARY pass=<n> fail=<n> skip=<n>
```

## Common Codex execution contract

For `RUN PHASE N HIL`:

```text
inspect current branch + git status
-> route to phase N dedicated test branch
-> read AGENTS.md + phase HIL plan/runbook
-> inspect before editing
-> clean/reconfigure build
-> flash connected ESP32-S3
-> monitor from reset
-> run phase test matrix independently
-> request only genuinely manual physical actions
-> report PASS / FAIL / SKIP with runtime evidence
```

Do not mark PASS from source inspection. Do not test older phases directly using whatever production branch is currently checked out.

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

## Recommended acceptance order when hardware returns

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

## What to answer when asked "làm gì tiếp theo?"

If hardware is unavailable:

1. P12-P16 HIL are deferred but routable through their dedicated commands/test branches.
2. Major feature coding is complete through Phase 16.
3. Do not propose Phase 17 automatically.
4. Useful work is documentation/review/test-harness preparation until hardware is available.

If hardware is available, start from Phase 12 HIL and follow the acceptance order above.

## Evidence discipline

- `IMPLEMENTED` — code exists;
- `STATIC REVIEW COMPLETE` — source review performed;
- `BUILD VERIFIED` — real ESP-IDF build evidence exists;
- `HIL PASS` — target evidence satisfies documented contract;
- `DEFERRED HIL` — hardware acceptance intentionally postponed.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
