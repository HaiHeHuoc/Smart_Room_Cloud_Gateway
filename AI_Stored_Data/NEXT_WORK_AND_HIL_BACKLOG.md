# Next Work + Deferred HIL Backlog

Updated: 2026-09-05
Authoritative development branch: `phase/16-audio-arbitration`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"** and HIL activation from any clean working branch.

## Current software state

```text
Phase 12 SW  -> COMPLETE / HIL PASS
Phase 13 SW  -> COMPLETE / HIL PASS
Phase 14 SW  -> COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Phase 15 SW  -> COMPLETE / BUILD VERIFIED / targeted HIL partial
Phase 16 SW  -> COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Major feature coding -> COMPLETE through Phase 16
```

Do not start Phase 17 automatically. The next project stage is acceptance, integration, hardening, and evidence-driven fixes unless Hải explicitly expands feature scope.

## Global Codex HIL routing

All Phase 12-16 HIL commands may be entered from the latest clean branch. Codex must inspect `git status`, route to the dedicated test branch, and read the phase HIL plan/runbook before editing, building, flashing, or monitoring.

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL -> test/phase16-audio-arbitration-hil
```

Never auto-stash, reset, delete, or test an older phase on an arbitrary production branch. All phases use PASS / FAIL / SKIP evidence discipline; expected logs are contracts, not observed hardware evidence.

## Phase acceptance state

### Phase 12

`test/xiaozhi-p2f-known-audio-e2e` is a closed HIL regression baseline. Rerun it only when an explicit regression requires it.

### Phase 13

`test/phase13-voice-assistant-hil` is a closed HIL regression baseline. Rerun it only when an explicit regression requires it.

### Phase 14

`phase/14-ptt-voice-mvp` and `test/phase14-ptt-voice-e2e-hil` carry the PTT voice baseline. Three GPIO38 turns reached audible Xiaozhi response playback in the recorded golden-path run. The later targeted regression proves boot/reconnect, capture, response wait, busy-response rejection, and playback completion, but the exact image still needs fresh audible confirmation and deferred fault-injection cases remain unexecuted.

### Phase 15

`phase/15-voice-assistant-ui` and `test/phase15-voice-ui-hil` are synchronized. The build-verified target trace covers boot/reconnect, actual capture, response wait, playback completion, and busy-response PTT rejection. Visible LCD `RECORDING`/duration, USER/ASSISTANT semantic text, latest-turn UX, recovery presentation, truncation, and UI resource stress remain pending HIL evidence.

### Phase 16

`phase/16-audio-arbitration` retains the production arbitration architecture; `test/phase16-audio-arbitration-hil` is its dedicated HIL branch. The combined operator-confirmed PTT/speaker evidence and 2026-09-04 automatic target matrix passed; see `PHASE16_HIL_EVIDENCE.md`. Long-duration/full-Gateway regression remains deferred.

## Production-vs-test fix policy

```text
Test harness/config/expected-log defect
-> fix on the test branch

Production component/architecture defect
-> fix on the owning production branch first
-> propagate forward to later production branches and their test branches
-> rebuild and retest the affected acceptance case
```

Never merge HIL/test-harness history as production feature history.

## Common HIL execution contract

```text
inspect branch + worktree
-> route to the dedicated test branch
-> read AGENTS.md + HIL plan/runbook
-> clean/reconfigure build
-> flash the connected ESP32-S3
-> monitor from reset
-> run the independent test matrix
-> request only genuine manual physical actions
-> report runtime-backed PASS / FAIL / SKIP
```

## Recommended next acceptance order

1. Keep Phase 16 as a closed HIL regression baseline.
2. On available hardware, complete Phase-15 visible UI/text HIL on its dedicated test branch.
3. Run the full Gateway/Firebase integration regression.
4. Preserve Phase 12/13 as regression baselines and Phase 14's recorded golden-path PASS; rerun them only for a relevant regression.
5. The next full Gateway/Firebase integration regression should cover Wi-Fi/provisioning, sensor, Firebase, GUI, SD, audio, Xiaozhi, simultaneous cloud/Xiaozhi traffic, repeated PTT, notification queueing, and critical-alarm preemption.

## Evidence discipline

- `IMPLEMENTED` — code exists.
- `STATIC REVIEW COMPLETE` — source review performed.
- `BUILD VERIFIED` — a real ESP-IDF build passed.
- `HIL PASS` — target evidence satisfies the documented contract.
- `TARGETED HIL PARTIAL` — only the named runtime cases have evidence.
- `DEFERRED HIL` — acceptance is intentionally not yet run.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
