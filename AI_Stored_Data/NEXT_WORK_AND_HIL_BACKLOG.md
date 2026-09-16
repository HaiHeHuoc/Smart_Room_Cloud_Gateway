# Next Work + Deferred HIL Backlog

Updated: 2026-09-16
Active Sprint-19 branch: `phase/19-local-web-storage-v1`
Phase-18.3/18.4 base HEAD: `bccd5a0754f393710f123b6bfc2a952cb9d46470` (`main_including_Firebase_security`)

Purpose: route future sessions to the highest-value next work without reopening
accepted phases or inventing validation evidence.

## Current software state

```text
Phase 12     COMPLETE / HIL PASS
Phase 13     COMPLETE / HIL PASS
Phase 14     SOFTWARE COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Phase 15     COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Phase 16     COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1   COMPLETE BASELINE / streaming HIL accepted / endurance pending
Phase 17     COMPLETE / read-only MCP voice HIL accepted
Phase 18     MCP CONTROLLED ACTIONS / IN PROGRESS
Phase 18.1   COMPLETE / build PASS / target HIL accepted by Hải on 2026-09-13
Phase 18.2   SOFTWARE INTEGRATED / TARGET HIL PENDING
18.2.1       Playback control + PTT suspension/auto-resume
              software implemented / build + host tests verified / HIL pending
18.2.2       Bounded playback start + voice SD audio selection
              software implemented / build + host tests verified / HIL pending
Phase 18.3   SUPERSEDED / absorbed into 18.2.2 / no duplicate production code
Phase 18.4   cloud.push_latest / software implemented / host tests verified /
             ESP-IDF build environment blocked / target HIL pending
Sprint 19    Local Web Control V1: SD Card File Manager / software hardened / build PASS / target HIL pending
Sprint 20    Local Web Control V2: Playback + Volume / PLANNED / NOT STARTED
Sprint 21    Local Web Control V3: Lights / PLANNED / NOT STARTED
Sprint 22    Local Web Control V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23    Local Web Control V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24    Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

Phase-18.2 source is integrated on the active branch. This does not close the
phase: real SD/I2S/audio/PTT/network timing and resource stability remain target
acceptance work.

## Immediate next work

Sprint 19 Prompts 1-3 are implemented and hardened on branch
`phase/19-local-web-storage-v1`. The local HTTP feature now has browse,
streamed transfer, file/folder mutations, path hardening, and LCD status
routing. A clean ESP-IDF 6.0.1 build passes, but browser/board/SD HIL has not
run. Do not treat Sprint 19 work as Phase-18 target acceptance or mark Sprint
18 closed.

The highest-value next work is now:

```text
1. Run the combined Phase-18.2.1 + 18.2.2 target HIL matrix.
2. Capture equivalent-checkpoint Internal/DMA/PSRAM and task-stack/resource trends.
3. Record failures/fixes against the exact tested source revision.
4. Close Phase 18.2 only after the required hardware/system evidence is accepted.
5. Start Phase 18.3 only when Hải explicitly requests it after/around the accepted gate.
6. Do not start Sprint 19-24 implementation until Hải explicitly requests it.
```

The preceding numbered suggestion is historical and fully superseded by this
current priority order:

```text
1. Run the pending Sprint-19 browser/board/SD HIL matrix from
   LOCAL_WEB_DASHBOARD_PLAN.md, including transfer interruption and SD remount.
2. Run the combined Phase-18.2.1 + 18.2.2 target HIL matrix.
3. Capture equivalent-checkpoint Internal/DMA/PSRAM and task-stack/resource trends.
4. Record failures/fixes against the exact tested source revision.
5. Close Phase 18.2 only after the required hardware/system evidence is accepted.
6. Do not start Sprint 20+ implementation without an explicit request.
```

Do **not** rerun Phase-18.1 light acceptance merely because later audio source
was merged. Phase 18.1 remains closed unless a concrete regression is found.

The older numbered suggestion to start Phase 18.3 is superseded: 18.3 is
historical traceability only and its bounded playback scope is already in 18.2.2.
The Sprint-19 checkout now has a clean serialized ESP-IDF 6.0.1 build PASS.
Target work still requires an attached ESP32-S3, mounted SD card, and a browser
on the existing LAN; the host used for the final review exposed only COM1.

## Phase 18.4 cloud.push_latest target matrix

`cloud.push_latest {}` is software implemented but has no target evidence.
After a clean ESP-IDF build, validate these bounded result and later-state cases:

- normal latest snapshot request: MCP returns `accepted=true` and
  `upload_complete=false`; later cloud status/logs determine success or failure;
- no latest telemetry or cloud worker not started: `not_ready`, with no HTTP;
- no IPv4: `offline`, with no accepted request or HTTP;
- repeated delivery before completion, active upload, and retry backoff:
  `busy`, with no second forced upload queued;
- terminal auth/configuration state: `invalid_state`;
- Wi-Fi loss after accepted scheduling: retained request follows existing
  reconnect/epoch/retry policy without duplicate transport;
- a newly accepted request may bypass only successful-upload pacing, never a
  retry backoff; and
- no credentials/tokens/payloads in MCP/log output, no LVGL work from callbacks,
  and stable Internal/DMA/PSRAM minima plus cloud task high-water mark.

## Phase 18.2 target HIL — priority matrix

## Voice Recording Critical Window target matrix

This refactor has host state-machine evidence only. On the exact flashed source,
record a baseline and after-change table for `frames_queued`, `frames_sent`,
`frames_dropped_queue_full`, `frames_dropped_stale`, I2S RX timeout/overflow,
Internal/DMA/PSRAM free/min/largest, voice/PTT/uplink/audio/task stack HWM, CPU,
log backlog/deferred cycles, cloud/sensor deferral counters, and SD active lease
trend.

Run 20--50 GPIO38 turns covering short and long utterances, rapid press/release,
the monitor report boundary, an existing log backlog/active SD logging, sensor
cadence, eligible periodic cloud work, and varied Wi-Fi latency. For each turn,
retain the single uplink timing summary and check:

- critical enters only after capture start and clears after release, cancel,
  network/Opus failure, or transport abort;
- normal workload has `frames_dropped_queue_full=0`, no unexpected I2S timeout
  or overflow growth, and no crash/WDT/deadlock;
- logger resumes without a monotonically growing backlog or leaked SD lease;
- deferred sensor/cloud/monitor work resumes without data-state corruption;
- an accepted `cloud.push_latest` and any in-flight cloud operation retain their
  existing behavior; and
- any remaining drop is classified as I2S, scheduling, uplink queue, Opus, or
  TLS/WebSocket/network rather than hidden by increasing buffers.

This is cooperative workload prioritization, not a hard-real-time guarantee.

### P0 — basic owner/state correctness

- WAV PLAY -> PAUSE -> RESUME near the retained committed position.
- WAV PLAY/PAUSE -> RESTART from frame zero.
- WAV PLAY/PAUSE -> STOP and then prove a fresh playback can start.
- Repeated PAUSE/STOP and invalid RESUME from IDLE.
- Pause near EOF and during prefetch/refill.
- Retained-recording control where available.
- Confirm Xiaozhi PCM/TTS remains non-resumable and terminates through its
  intended abort/cancel path.

### P0 — GPIO38 PTT policy

- Local resumable playback -> GPIO38 hold -> safe temporary suspension -> same
  held press reaches capture -> Xiaozhi response -> guarded auto-resume.
- Fast release during suspension -> no late microphone start; local source
  returns to the correct state.
- Temporary suspension plus explicit STOP -> remain IDLE after response.
- Temporary suspension plus explicit PAUSE -> remain USER-paused.
- Temporary suspension plus explicit RESUME/RESTART -> apply after TTS according
  to current policy without overlapping the acknowledgement unnecessarily.
- GPIO38 during Xiaozhi TTS -> old response terminates; same press may start the
  new turn only after the required cleanup/fence; old speech never resumes.

### P0 — bounded SD track catalog / playback start

- Valid catalog and exact logical ID playback.
- Unknown/overlong ID rejection.
- Empty directory.
- More than 12 valid tracks -> deterministic bounded set with `truncated=true`.
- Non-WAV, unsupported/corrupt WAV, directory entries, and unsafe names.
- SD unavailable at boot then recovery/remount.
- SD/media error while scanning -> old/partial invalid snapshot is not published.
- List/play while the catalog worker scans.
- Confirm an accepted track request either reaches the owner WAV-start evidence
  or later reports an owner failure; do not equate request scheduling with
  audible playback.

### P0 — response generation / transport fence

Run 20-50 GPIO38 response interruptions and play/stop alternations. For local
response aborts, verify the expected transport-fence sequence and that capture
is never authorized before a fresh READY generation. A stop/start/drain/fence
failure must remain fail-closed.

Confirm that no stale `PCM_STREAM_REJECTED`, old response abort, old speech, or
old queued downlink work enters the next response generation after cancellation.

### P1 — resource and endurance evidence

At equivalent lifecycle/workload checkpoints capture:

```text
Internal free / minimum / largest
DMA free / minimum / largest
PSRAM free / minimum / largest
catalog-worker stack HWM
WAV-reader stack HWM
voice/PTT/uplink/downlink stack HWM
arbiter/audio-manager stack HWM where observable
CPU
SD lease count/trend
pause/resume/PTT-authorization/play-start latency
```

Run repeated pause/resume, PTT interruption, catalog/list/play, SD failure and
recovery cycles and verify there is no monotonic heap/resource loss, stale task,
stale SD lease, deadlock, watchdog, Guru Meditation, or uncontrolled playback.

## Phase 18.2 software evidence already recorded

18.2.1:

```text
audio-manager host tests       PASS
voice-assistant host tests     PASS
Xiaozhi/provider host tests    PASS
ESP-IDF 6.0.1 build            PASS
target HIL                     PENDING / NOT CLAIMED
```

18.2.2:

```text
audio-manager host tests       PASS
voice-assistant host tests     PASS
Xiaozhi/provider host tests    PASS
ESP-IDF 6.0.1 build            PASS
target HIL                     PENDING / NOT CLAIMED
firmware size                  0x26fb90
free app partition             0x190470 (39%)
DIRAM build-size snapshot      167376 / 341760 bytes (48.97%)
```

Host/build evidence does not emulate FATFS/SD removal, FreeRTOS interleavings,
I2S/MAX98357A, TLS/network timing, audible output, or runtime resource minima.

## Canonical roadmap discrepancy

`XIAOZHI_IMPLEMENTATION_ROADMAP.md` now records Phase 18.3 as superseded by
18.2.2 and Phase 18.4 as software implemented with HIL pending. Older lines
that say 18.2-18.4 are unstarted are historical planning context only.

Current execution status follows current source and Phase-18 records. Preserve
the Phase 18.3 number and do not infer Phase-18.4 HIL from host evidence.

## Future roadmap routing

After current Sprint-18 work and only when Hải explicitly starts the next item:

```text
Sprint 19 -> Local Web Control V1: SD Card File Manager
Sprint 20 -> Local Web Control V2: Playback + Volume
Sprint 21 -> Local Web Control V3: Lights
Sprint 22 -> Local Web Control V4: Dashboard + System Status
Sprint 23 -> Local Web Control V5: Scenes + Logs + Diagnostics
Sprint 24 -> Wake Word + Advanced Voice UX
```

Do not reintroduce Wake Word as Sprint 19. Web/LCD remain sibling frontends over
existing manager/service ownership. Web UI must not configure/control Wi-Fi.
Advanced OTA/factory-management flows remain outside the approved Local Web
scope.

## Deferred work outside Phase 18.2 closure

These remain useful but are not substitutes for Phase-18.2 acceptance:

- delayed-first-PCM / streaming regression on the current voice path;
- Phase-16/16.1 long-duration arbitration/streaming endurance;
- repeated PTT/resource-trend work beyond the Phase-18.2 bounded matrix;
- long-duration Firebase/cloud + Xiaozhi simultaneous traffic;
- relevant visible UI/text regression only when a defect touches that path;
- release/documentation/portfolio closure after feature and acceptance work.

## PTT / TLS retained facts

Current source uses dynamic TLS buffers in PSRAM, 1 KiB outbound TLS records,
and a 20 KiB total/largest-contiguous PSRAM gate before a PTT turn starts.

Hải confirmed repeated PTT/TLS smoke PASS during Phase-18.1 acceptance. Do not
list the old basic smoke as an unresolved Phase-18.1 blocker. Phase-18.2 still
needs its own real timing/fence/PTT interaction evidence because it adds new
audio-control concurrency and response-generation behavior.

## Streaming downlink — focused regression still deferred

Current source uses a 7.68-second bounded PSRAM ingress ring and a 0.96-second
normal prefill. The 5-second prefill wait starts only after the first PCM packet,
not at `TTS_START`.

A focused delayed-first-PCM regression remains useful because timing semantics
changed after the older accepted Phase-16.1 baseline. Do not claim it PASS
unless a newer target run explicitly records it.

## Historical HIL routing

Older accepted branches remain regression references, not current production
source:

```text
RUN PHASE 12 HIL   -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL   -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL   -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL   -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL   -> test/phase16-audio-arbitration-hil
RUN PHASE 16.1 HIL -> phase/16.1-streaming-downlink
```

Inspect the actual worktree before using an old test branch. Never auto-stash,
reset, discard, delete, or generalize old-branch evidence to a newer source tree.

## Evidence vocabulary

- `IMPLEMENTED` — source exists.
- `STATIC REVIEW COMPLETE` — source review performed.
- `BUILD VERIFIED` — an actual relevant ESP-IDF build passed.
- `HIL PASS` / `HIL ACCEPTED` — target evidence satisfies the named contract.
- `TARGETED HIL PARTIAL` — only named runtime cases have evidence.
- `PENDING` — no current evidence yet.

A build or HIL result belongs to the source/checkpoint actually tested. A later
merge or behavior-preserving refactor does not become a new hardware run unless
target evidence is actually recorded for that source.
