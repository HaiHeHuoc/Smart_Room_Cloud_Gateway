# Current Working Checkpoint

Purpose: compact V2 release-hardening handoff. Current source, `AGENTS.md`,
canonical documents, and observed build/HIL evidence remain authoritative.

Updated: 2026-09-28

## Active V2 source

- V2 baseline: `44e6feb23f3358917171b6b326b56fdec8ae7ff3`.
- V2-R08 candidate: `v2r08` commit `1511d7e`.
- Sprint 24 Wake Word + Advanced Voice UX remains **SUSPENDED FOR V2 RELEASE**.
- No target HIL is recorded for V2-R08.

## V2-R08 lifecycle recovery

`voice_assistant` now owns staged startup across audio manager, both arbiters,
voice task, UI, playback policy, PTT, uplink/downlink, GPIO38, and Xiaozhi
session. A failed stage reverses successful owners in dependency order.

Each owner uses bounded cooperative stop/deinit. If any cleanup times out or
fails, rollback stops immediately, retains dependencies of the possibly live
owner, marks retry unsafe, and transitions the application to its fatal path.
It does not deinitialize audio resources underneath a still-running task.

`smart_room_app` uses explicit NEEDS_START / RETRY_WAIT / READY / FATAL boot
states, with at most three 1/2/4-second retries only after complete rollback.

## Validation performed

- `voice_assistant` host suite: PASS, including transaction rollback, transient
  retry, repeated cleanup, and fail-closed cleanup-timeout behavior.
- `audio_manager` host suite: PASS, including arbiter lifecycle ownership.
- `xiaozhi_foundation` host suite: PASS.
- `system/common` VRC host suite: PASS.
- `git diff --check`: PASS before commit.
- Clean exported ESP-IDF v6.0.1 `esp32s3` build: PASS in `build_v2r08`.
  Application: `0x296640` of `0x400000`; free `0x1699c0` (35%).

## Remaining V2 evidence

- V2-R01 remains measure-first: a slow synchronous provider send can fill the
  bounded 16-frame PCM queue and deliberately drop PCM to protect I2S.
- V2-R02/R03/R04 require target measurement of scheduler pressure, optional SD
  work, and in-flight cloud HTTPS/TLS overlap.
- Prompt 6 must capture RX overflow/timeout deltas, `queue_drops`,
  `stale_drops`, queue peak, first PCM/Opus timing, max encode/send duration,
  task/stack state, PSRAM state, and user-observed speech quality.

## Next action

Integrate the V2-R08 commit into the requested branch, then run Prompt 6 HIL
against that exact final source. Do not start Sprint 24 automatically.
