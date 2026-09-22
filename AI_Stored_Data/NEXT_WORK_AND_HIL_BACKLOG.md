# Next Work + Deferred HIL Backlog

Updated: 2026-09-16
Active branch: `main_including_Firebase_security`
Sprint-18 closure authority: explicit user acceptance by Hải on 2026-09-16
Sprint-19 Local Web Storage V1 source is integrated; target HIL remains pending.

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
Sprint 18    COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16
Phase 18.1   COMPLETE / build PASS / target HIL accepted
Phase 18.2   COMPLETE / USER ACCEPTED
18.2.1       COMPLETE / playback control + PTT suspension/auto-resume
18.2.2       COMPLETE / bounded playback start + voice SD audio selection
Phase 18.3   COMPLETE / superseded + absorbed into 18.2.2 / no duplicate code
Phase 18.4   COMPLETE / cloud.push_latest / USER ACCEPTED
Sprint 19    Local Web Control V1: SD Card File Manager / SOURCE INTEGRATED /
             BUILD VERIFIED / TARGET REDEPLOY AND HIL PENDING
Sprint 20    Local Web Control V2: Playback + Volume / PLANNED / NOT STARTED
Sprint 21    Local Web Control V3: Lights / PLANNED / NOT STARTED
Sprint 22    Local Web Control V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23    Local Web Control V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24    Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

## Closure rule for Sprint 18

Hải explicitly closed all Phase 18 work from 18.1 through 18.4 on 2026-09-16.
Older Phase-18 HIL matrices remain useful regression checklists but are no longer
blocking acceptance or the roadmap transition.

Do not rewrite missing historical evidence:

- 18.1 has recorded build/HIL acceptance.
- 18.2.1/18.2.2 retain their recorded host/build evidence.
- 18.3 is complete because its historical scope is already implemented inside
  18.2.2; no duplicate feature is required.
- 18.4 retains the historical fact that one checkout-specific ESP-IDF build was
  blocked by missing `IDF_PATH`; user acceptance closes the phase but does not
  transform that old attempt into a build PASS.
- Voice Recording Critical Window is complete by explicit user acceptance; host
  state-machine evidence remains the recorded automated evidence for that
  checkpoint.

Future sessions must treat a regression checklist as optional/deferred work,
not as evidence that Sprint 18 is still open.

## Immediate next work

Sprint 19 Local Web Storage V1 is integrated. It provides browse, streamed
transfer, file/folder mutations, path hardening, LCD status routing, correct
FAT32 capacity/large-file metadata, download filenames, and a 20 MiB upload
limit. It does not alter Sprint-18 acceptance.

```text
Sprint 19 — Local Web Control V1: SD Card File Manager
```

Run the pending browser/board/SD HIL against the merged revision:

```text
1. Flash the merged firmware and verify `/api/storage/status` reports coherent
   non-zero capacity values.
2. Verify a 2-4 GiB FAT32 file reports a plausible size and downloads with the
   requested filename.
3. Verify upload of a non-empty file up to 20 MiB, transfer interruption, and
   SD remount/recovery behavior.
4. Record target results against the exact merged revision before starting
   Sprint 20.
```

## Deferred regression backlog — non-blocking

### Former Phase 18.2 audio/PTT/SD matrix

Useful future regression cases include:

- WAV PLAY -> PAUSE -> RESUME / RESTART / STOP continuity;
- retained-recording control where available;
- GPIO38 local playback suspension -> same held press -> capture -> response ->
  guarded auto-resume;
- fast release before capture and explicit STOP/PAUSE/RESUME/RESTART overrides;
- GPIO38 during Xiaozhi TTS with no stale response crossing generations;
- valid/unknown track IDs, empty/over-limit catalog, unsupported/corrupt WAVs;
- SD unavailable/remount/recovery and scan/list/play races;
- repeated response interruption/transport-fence cycles;
- equivalent-checkpoint Internal/DMA/PSRAM, relevant task stack HWM, CPU,
  SD-lease trend, and playback/PTT latency.

These checks can improve release confidence but no longer gate Sprint-18 status.

### Phase 18.4 cloud regression

Useful future cases:

- normal `cloud.push_latest {}` scheduling;
- `not_ready`, `offline`, `busy`, `invalid_state`, and local scheduling failure;
- Wi-Fi loss after accepted scheduling;
- successful-period bypass without retry-backoff bypass;
- no credential/token/payload leakage;
- stable cloud task and memory/resource trends.

`accepted=true` continues to mean request scheduling, not upload completion.

### Voice Recording Critical Window regression

Useful target measurements if revisited:

```text
frames_queued / frames_sent
frames_dropped_queue_full / frames_dropped_stale
I2S RX timeout/overflow
Internal/DMA/PSRAM free/min/largest
voice/PTT/uplink/audio task stack HWM
CPU
log backlog/deferred cycles
cloud/sensor deferral counters
SD active lease trend
```

Run repeated short/long GPIO38 turns, rapid press/release, log backlog, monitor
boundary, sensor cadence, periodic cloud work, and varied Wi-Fi latency if a
future regression, release qualification, or performance investigation needs
this evidence.

The state remains cooperative workload prioritization, not a hard-real-time
guarantee.

## Other deferred work

- Phase-16/16.1 long-duration arbitration/streaming endurance;
- delayed-first-PCM focused streaming regression;
- long-duration Firebase/cloud + Xiaozhi simultaneous-traffic regression;
- release-level target smoke and portfolio evidence when preparing a tagged
  release.

None of these automatically reopens an accepted sprint.

## Future roadmap routing

When Hải explicitly starts the next item:

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

## Evidence vocabulary

- `IMPLEMENTED` — source exists.
- `STATIC REVIEW COMPLETE` — source review performed.
- `BUILD VERIFIED` — an actual relevant ESP-IDF build passed.
- `HIL PASS` / `HIL ACCEPTED` — target evidence satisfies the named contract.
- `USER ACCEPTED` — Hải explicitly closes the project checkpoint; this is a
  project status decision and does not invent a build/HIL event that was not
  separately recorded.
- `TARGETED HIL PARTIAL` — only named runtime cases have evidence.
- `PENDING` — no current evidence yet.

Evidence belongs to the source/checkpoint actually tested. User acceptance and
technical evidence must remain distinguishable in future summaries.
