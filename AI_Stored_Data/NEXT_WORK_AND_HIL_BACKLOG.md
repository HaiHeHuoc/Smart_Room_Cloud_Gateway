# Next Work + Deferred HIL Backlog

Updated: 2026-09-12
Snapshot source branch: `main_including_Firebase_security` at `34a01c8934dd01555f75ecef7925c55aeec3a4df`
Purpose: cross-session/Codex routing for **"hiện tại nên làm gì tiếp theo?"** and HIL activation from any clean working branch.

## Current software state

```text
Phase 12 SW   -> COMPLETE / HIL PASS
Phase 13 SW   -> COMPLETE / HIL PASS
Phase 14 SW   -> COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Phase 15 SW   -> COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Phase 16 SW   -> COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1 SW -> IMPLEMENTED / BUILD VERIFIED / automated HIL PASS / audible recovery confirmed / endurance pending
Phase 17      -> IN PROGRESS; sensor-answer read-only MCP slice merged / BUILD VERIFIED / user-confirmed voice HIL accepted
Phase 18      -> NOT STARTED
Phase 19      -> NOT STARTED
```

Phase 17 was explicitly started. The first production read-only MCP vertical slice,
`smart_room.get_current_temperature_humidity`, was merged from
`phase/17-xiaozhi-sensor-answer` into `main_including_Firebase_security` by merge
commit `34a01c8934dd01555f75ecef7925c55aeec3a4df`. The accepted evidence records
three correct user-confirmed voice-query cases. Treat Phase 17 overall as
**IN PROGRESS**, not complete; remaining read-only MCP scope must be implemented
and accepted separately.

Do not start Phase 18 automatically. Continue Phase 17 only when Hải explicitly
requests the next read-only MCP slice.

## Global Codex HIL routing

All Phase 12-16 HIL commands may be entered from the latest clean branch. Codex must inspect `git status`, route to the dedicated test branch, and read the phase HIL plan/runbook before editing, building, flashing, or monitoring.

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL -> test/phase16-audio-arbitration-hil
RUN PHASE 16.1 HIL -> phase/16.1-streaming-downlink
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

Phase 15 is closed. Hardware/manual acceptance was confirmed by the user on 2026-09-06, and the current unattended Xiaozhi UI lifecycle target regression passed all seven cases on `test/xiaozhi-ui-lifecycle-hil` at `fc5a3fa`. Retain `test/phase15-voice-ui-hil` as a regression route when a future UI/text defect requires it; do not list Phase 15 as pending acceptance work.

### Phase 16

`phase/16-audio-arbitration` retains the production arbitration architecture; `test/phase16-audio-arbitration-hil` is its dedicated HIL branch. The combined operator-confirmed PTT/speaker evidence and 2026-09-04 automatic target matrix passed; see `PHASE16_HIL_EVIDENCE.md`. Long-duration/full-Gateway regression remains deferred.

### Phase 16.1

`phase/16.1-streaming-downlink` replaces Xiaozhi's full-response PSRAM/SD/WAV handoff with bounded decoded PCM16 ingress to the manager-owned playback ring. The public foundation callback remains copy-only; the downlink worker is the single decoder/producer and `audio_manager` remains the sole I2S/DMA owner. The automated target matrix and audible recovery are accepted. The remaining Phase-16.1 item is endurance coverage, not first-pass streaming acceptance.

### Phase 17

The first accepted read-only MCP slice is the Smart Room temperature/humidity query tool. It reads only a valid, non-stale copied sensor snapshot through the composition root and performs no device-side state change. Build and user-confirmed voice HIL are accepted for that slice. Remaining Phase-17 work is additional read-only MCP capability and its acceptance, not Phase-18 controlled actions.

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

1. Keep Phase 16 and Phase 16.1 as accepted regression baselines; run endurance only when explicitly scheduled.
2. Continue Phase 17 with the next bounded read-only MCP vertical slice; `network.get_status` / `smart_room.get_network_status` is the current recommended next candidate, subject to current public API inspection.
3. Keep Phase 18 controlled actions NOT STARTED until Phase 17 is explicitly closed.
4. Run full Gateway/Firebase integration regression as appropriate, covering Wi-Fi/provisioning, sensor, Firebase, GUI, SD, audio, Xiaozhi, simultaneous cloud/Xiaozhi traffic, repeated PTT, notification queueing, and critical-alarm preemption.
5. Preserve Phase 12/13/15 as regression baselines and Phase 14's recorded golden-path PASS; rerun them only for a relevant regression.

## Evidence discipline

- `IMPLEMENTED` — code exists.
- `STATIC REVIEW COMPLETE` — source review performed.
- `BUILD VERIFIED` — a real ESP-IDF build passed.
- `HIL PASS` — target evidence satisfies the documented contract.
- `TARGETED HIL PARTIAL` — only the named runtime cases have evidence.
- `DEFERRED HIL` — acceptance is intentionally not yet run.

`AI_Stored_Data/` is cross-session metadata only and must never become a firmware/build dependency.
