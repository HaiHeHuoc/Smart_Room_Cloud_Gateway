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
Phase 16 SW -> IN PROGRESS / 16-B complete
```

Continue Phase 16 one checkpoint at a time and only after Hải says `tiếp tục`.

## Global Codex HIL routing

Commands may be entered from the latest branch, but Codex must route to the dedicated test branch before testing:

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
```

Before checkout, inspect git status. If local work could be lost, stop and report; never auto-stash/reset/delete. If a required test branch/harness does not exist, report the prerequisite instead of testing on the current production branch.

## Deferred HIL

- Phase 12: `test/xiaozhi-p2f-known-audio-e2e`, activation `RUN PHASE 12 HIL`.
- Phase 13: `test/phase13-voice-assistant-hil`, activation `RUN PHASE 13 HIL`.
- Phase 14: plan ready; dedicated `test/phase14-ptt-voice-e2e-hil` prerequisite must be verified/created before execution.
- Phase 15: `test/phase15-voice-ui-hil`, activation `RUN PHASE 15 HIL`.

All remain DEFERRED when hardware is unavailable.

## Phase 16 — active software work

Production branch: `phase/16-audio-arbitration`
Progress: `AI_Stored_Data/PHASE16_PROGRESS.md`

Goal: centralized multi-client capture/playback arbitration while `audio_manager` remains the sole I2S/DMA owner.

Current checkpoint:

```text
16-A Audio client/request model                 COMPLETE
16-B Playback arbitration runtime               COMPLETE
16-C Capture arbitration runtime                NEXT
16-D Priority/preemption/queue hardening        pending
16-E Xiaozhi + notification/alarm integration   pending
16-F Final review/HIL plan                      pending
```

16-B added a bounded playback arbiter with one current + one pending WAV request, copied request/client metadata, `REJECT`, `QUEUE`, and cooperative `PREEMPT_LOWER_PRIORITY` behavior. It coordinates only through public `audio_manager_play_wav()` / `audio_manager_stop_playback()` and copied manager status; it owns no I2S or source hardware.

Legacy Xiaozhi/notification/alarm callers are intentionally not migrated yet. Unknown legacy playback is never preempted because its client/interruptibility metadata is unavailable. Client migration is deferred to 16-E.

Known 16-B follow-ups:

- arbiter stop/deinit lifecycle API not implemented yet;
- recorded-audio playback is not promoted into request arbitration yet;
- no build/HIL evidence yet.

## What to answer when asked "làm gì tiếp theo?"

If hardware is unavailable, answer in this order:

1. Phase 12 HIL deferred / Codex-ready.
2. Phase 13 HIL deferred / Codex-ready.
3. Phase 14 HIL deferred / dedicated test-branch prerequisite.
4. Phase 15 HIL deferred / test branch ready.
5. Phase 16 active: 16-A and 16-B complete.
6. Next software checkpoint: **16-C — Capture Arbitration Runtime**, only after explicit `tiếp tục`.

Concise state:

```text
P12 HIL -> DEFERRED
P13 HIL -> DEFERRED
P14 HIL -> DEFERRED / branch prerequisite
P15 HIL -> DEFERRED / test branch ready
P16 SW  -> 16-B COMPLETE
Next SW -> 16-C after `tiếp tục`
```

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
