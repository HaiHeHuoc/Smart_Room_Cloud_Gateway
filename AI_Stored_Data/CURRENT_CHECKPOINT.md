# Current Working Checkpoint

Purpose: compact V2 release-hardening handoff. Current source, `AGENTS.md`,
canonical documents, and observed build/HIL evidence remain authoritative.

Updated: 2026-09-29

## Active V2 source

- V2 baseline: `44e6feb23f3358917171b6b326b56fdec8ae7ff3`.
- V2-R08 lifecycle recovery: commit `1511d7e`.
- Exact V2 promotion source: `release/V2.0.0` through `4dc89c2`
  (`Adjust UI`), carried by `integration/v2-to-main` for the
  `main_including_Firebase_security` review/merge path. Until that PR merges,
  `release/V2.0.0` remains the source of record; after it merges, the target
  contains this V2 source chain.
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

## V2 dashboard Web IPv4 usability change

- Committed release-source change: `4dc89c2` makes `app_gui` reuse the copied
  `ui_wifi_status_t` snapshot to render a centered `Web: <IPv4>` bottom row
  on `SENSOR_DASHBOARD`. It shows `Web: --` without an address, including
  after a disconnect, and restores the cached address after screen recreation.
- The new separator is above the row and the existing vertical divider ends at
  that separator. No task, queue, Wi-Fi query, audio, or Xiaozhi code changed.
- Source contract checks and `git diff --check` passed. A clean exported
  ESP-IDF v6.0.1 `idf.py build` passed after regenerating the ignored stale
  default `build/` cache for `esp32s3`; the application uses `0x291740` of
  `0x400000`, leaving `0x16e8c0` (36%). A prior COM4 flash/boot reached the
  dashboard, but that is boot sanity only; no formal LCD visual HIL is recorded.

## APP_LOG ANSI console color

- `sdkconfig.defaults` now sets `CONFIG_LOG_COLORS=y`, so the existing
  `APP_LOG` console sink keeps ANSI level colors after a clean ESP-IDF
  configuration. No logging frontend or backend code changed.
- Regenerated ignored `sdkconfig` confirms the setting. A clean exported
  ESP-IDF v6.0.1 `esp32s3` build passed: application `0x2966c0` of
  `0x400000`, with `0x169940` (35%) free. Target terminal/HIL output was not
  run.

## V2 Local Web Xiaozhi Remote PTT exception

- Explicit late-V2 feature: Local Web is a second PTT frontend only. GPIO38
  and Web enter the same `voice_assistant_ptt` policy, playback arbitration,
  `voice_assistant_uplink`, `audio_manager`, and Xiaozhi pipeline; browser
  microphone/PCM/Opus/WebRTC are not supported.
- The public PTT contract tracks `GPIO`/`WEB` source ownership. A Web start
  returns a PTT generation; a matching client ID and generation are required
  for keepalive/stop, so a stale command cannot affect a newer turn. A second
  source receives BUSY and cannot preempt. Web renews every 2 s; the PTT task
  releases an absent heartbeat after 8 s using `esp_timer_get_time()`.
- Local Web exposes copied status/transcripts and bounded HTTP start,
  keepalive, and stop routes; it owns no I2S, DMA, capture, Opus, or provider
  resource. Pointer release/cancel/blur/hide/page-hide attempt STOP; the
  device lease remains the fallback.
- Relevant voice and Local Web host suites PASS; `git diff --check` PASS;
  exported ESP-IDF v6.0.1 `esp32s3` build PASS. Application `0x299b60` of
  `0x400000`, leaving `0x1664a0` (35%). No flash/browser/board HIL was run.

## Remaining V2 evidence

- V2-R01 remains measure-first: a slow synchronous provider send can fill the
  bounded 16-frame PCM queue and deliberately drop PCM to protect I2S.
- V2-R02/R03/R04 require target measurement of scheduler pressure, optional SD
  work, and in-flight cloud HTTPS/TLS overlap.
- Prompt 6 must capture RX overflow/timeout deltas, `queue_drops`,
  `stale_drops`, queue peak, first PCM/Opus timing, max encode/send duration,
  task/stack state, PSRAM state, and user-observed speech quality.

## Next action

After the V2 integration PR is manually merged, run the expanded Prompt-6
Web/GPIO PTT HIL matrix against the resulting target source, including lease
expiry, stale generation, and recording-critical contention. Do not start
Sprint 24 automatically.
