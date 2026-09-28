# Current Working Checkpoint

Purpose: compact V2 release-hardening handoff. Current source, `AGENTS.md`,
and actual build/HIL evidence remain authoritative.

Updated: 2026-09-28

## Active V2 baseline

Branch: `release/V2.0.0`

Source branch: `main_including_Firebase_security`

Exact source and release HEAD: `44e6feb23f3358917171b6b326b56fdec8ae7ff3`
(`merge: integrate PTT uplink robustness fix`). The release branch was created
directly from that SHA; no merge, rebase, reset, push, commit, or production
firmware change was made during Prompts 1-5.

Sprint 24 Wake Word + Advanced Voice UX is **SUSPENDED FOR V2 RELEASE**. It is
neither complete nor cancelled and may resume only after explicit Hai
instruction. V2 permits only evidence-driven stabilization and validation.

## Prompt 2 audit result

Static source/runtime-policy audit of the full GPIO38 PTT recording chain is
complete: GPIO ISR/task -> PTT policy -> session/capture arbiter ->
`audio_manager` I2S/DMA -> stream callback -> bounded PCM queue -> Opus ->
managed Xiaozhi/WebSocket transport.

Confirmed strengths:

- `audio_manager` priority 7 is the sole I2S/RX/DMA owner; its stream tap and
  registered callback perform no allocation, filesystem, network, or logging.
- The callback copies one 256-sample frame and uses zero-wait enqueue to the
  one-time PSRAM-backed 16-frame PCM queue; it cannot block I2S.
- GPIO ISR only notifies its worker. Capture arbiter and PTT mutex waits are
  finite; provider send runs outside the session lock; no product source
  `portDISABLE_INTERRUPTS` was found.
- `VOICE_RECORDING_CRITICAL` enters only after capture starts, is
  generation-guarded, and dispatches its three listeners outside its spinlock.
  Performance monitor, persistent log writer, periodic cloud work, DHT reads,
  and heavy Local Web upload/download safe points defer without suspending
  Wi-Fi, TCP/IP, ESP system, or scheduler tasks.

Primary source-confirmed risk (V2-R01, P1 architectural risk, not an observed
HIL failure): one `voice_uplink` worker both encodes and synchronously sends.
The WebSocket send wrapper caps a formerly infinite wait at 8 s, but a slow
send can fill the approximately 256 ms PCM queue and deliberately drop frames
to protect I2S. Do not add another queue/task unless V2 HIL records
`queue_drops > 0` or full queue pressure while I2S RX remains clean.

Secondary measured risks:

- V2-R02: unpinned audio/uplink versus CPU0 Wi-Fi/ESP timer and unpinned
  high-priority TCP/IP requires per-core/task runtime data, not static pinning.
- V2-R03: Local Web icon transfer (whitelisted name but no file-size limit),
  bounded directory listing (64 scan/32 result), mutations, catalog scans, and
  an already in-progress log flush are not universally VRC-stoppable. They do
  not share capture locks; measure SD contention before gating more work.
- V2-R04: an in-flight cloud HTTPS/TLS request intentionally completes during
  VRC. It must not be forcibly interrupted; measure concurrent CPU/PSRAM and
  network pressure.
- Provider task priority/core and actual task stack high-water marks remain
  runtime facts, not source-proven facts.

## Prompt 3 disposition

Prompt 3 re-verified every Prompt-2 finding against this unchanged source.
None is classified REQUIRED or JUSTIFIED for a production change without target
measurements. Therefore no firmware behavior, priority, core affinity, queue,
DMA, provider, or VRC policy was changed.

- V2-R01 remains **MEASURE FIRST**: synchronous send can fill the bounded PCM
  queue, but source counters protect I2S and no HIL drop has been recorded.
- V2-R02/R03/R04 remain **MEASURE FIRST**: scheduler, optional SD work, and
  in-flight cloud HTTPS require the concurrent-load evidence below.
- V2-R05/R06 are **DO NOT CHANGE** pending runtime evidence.
- V2-R07 is an unrelated Xiaozhi catalog host-test mismatch; it is outside
  recording hardening and must be reconciled in a later validation prompt.

## Prompt 4 review

Prompt 4 found no source-level P0 issue and made no production firmware
change. The only bounded correction is in
`components/application/xiaozhi_foundation/test/host/run_tests.ps1`: its stale
check for a removed `stem_length` implementation was replaced with checks for
the current bounded base-ID and suffix-overflow guards. The full host suite now
passes.

V2-R08 lifecycle recovery is now implemented in the current dirty worktree.
`voice_assistant` owns a staged transaction across audio, both arbiters, voice,
UI, playback policy, PTT, uplink/downlink, GPIO38, and Xiaozhi session. Every
attempted stage is reversed in dependency order on a failure. Each owner uses a
bounded cooperative stop/deinit. A cleanup error stops further reverse cleanup
so dependencies of a possibly live owner remain allocated; the result is
retry-unsafe and fails closed rather than risking use-after-free.
`smart_room_app` now tracks explicit READY/RETRY_WAIT/FATAL states and uses at
most three 1/2/4-second retries only after confirmed complete rollback.

System review confirms the current ownership boundaries: `audio_manager` owns
I2S/DMA; voice owns session/uplink orchestration; provider handles remain in
foundation; Local Web is a manager frontend; UI/LVGL calls remain UI-owned;
Wi-Fi owns Station lifecycle; SD leases protect mount recovery. No product
source task suspension or direct non-UI LVGL call was found.

## Validation actually performed

- Static source/concurrency review and `git diff --check`: PASS.
- `system/common` host tests: PASS, including recording-critical production
  boundary checks.
- `voice_assistant` host tests: PASS, including PTT/downlink interruption,
  response-epoch, and post-abort transport-fence boundaries.
- `audio_manager` host tests: PASS, including playback control, WAV resume, and
  arbiter lifecycle ownership.
- `local_web_server` host path-policy tests: PASS.
- `xiaozhi_foundation` host suite: PASS after the static predicate correction;
  MCP/provider, catalog-cache, and transport-fence boundaries pass.
- V2-R08 lifecycle host suite: PASS for representative staged failure doubles.
  It verifies reverse cleanup, a safe transient retry, repeated cleanup, and
  fail-closed cleanup timeout behavior: no lower dependency is deinitialized
  below the owner that did not stop, and retry is rejected.
- Clean exported ESP-IDF v6.0.1 `esp32s3` build: PASS in the isolated
  `build_v2r08` directory. It compiled and linked the V2-R08 owners. The
  application binary is `0x296640` in the `0x400000` factory partition,
  leaving `0x1699c0` (35%) free. The old
  default `build/` directory has a stale bootloader CMake cache but was not
  modified or relied upon for this PASS.
- Clean exported ESP-IDF build: PASS for `esp32s3` with ESP-IDF v6.0.1. The
  application uses `0x2957c0` of its `0x400000` factory partition, leaving
  `0x16a840` (35%) free. Target HIL concurrent-load test: NOT RUN.

## Required V2 HIL evidence

For normal, sustained, weak/variable-network, Local Web SD, and cloud-overlap
PTT turns, capture: RX overflow/timeout deltas; `queue_drops`, `stale_drops`,
and peak depth; capture-to-first PCM/Opus; max encode/send duration; per-core
CPU/task state and stack high-water; Wi-Fi/TCP events; PSRAM free/largest block;
and audio quality/recognition result.

## Prompt 5 software-release result

All discovered self-contained host suites pass: Local Web, MCP adapter, voice
assistant, Xiaozhi foundation, audio manager, cloud manager, SD card, and
system/common VRC production-boundary checks. Firebase PowerShell scripts were
not run because they are interactive or use environment-supplied credentials
and external Firebase state, rather than being self-contained host suites.

The source/static review reconfirmed bounded recording ownership and found no
product-source task suspension or blanket interrupt control. `git diff --check`
passes; no staged changes, tracked build artifacts, case-colliding paths, or
literal credentials were found by manual tracked-source scanning. `gitleaks`
and `trufflehog` are unavailable, so those scanners were not claimed. The only
code/test correction remains the Prompt-4 host predicate update for the current
bounded Xiaozhi catalog-ID guards; no product behavior changed.

**Software release gate: V2-R08 software gate PASS.** Source recovery,
transactional lifecycle host tests, relevant existing host suites, and an
isolated exported full build all pass. No P0 issue was found. Hardware/HIL is
not claimed.

V2-R01 remains measure-first (synchronous uplink send can deliberately drop
bounded PCM to protect I2S); V2-R02/R03/R04 require the defined HIL scheduler,
SD, and cloud-overlap measurements. Do not add a sender queue, change affinity
or priority, widen VRC, or interrupt cloud/TLS without that evidence. Sprint 24
remains suspended.

## Next action

Prompt 6 HIL remains deferred and must not start automatically; Sprint 24
remains suspended.
