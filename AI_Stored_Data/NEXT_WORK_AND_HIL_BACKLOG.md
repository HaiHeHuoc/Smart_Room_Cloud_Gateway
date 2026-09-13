# Next Work + Deferred HIL Backlog

Updated: 2026-09-13
Active branch: `main_including_Firebase_security`
Current production/source baseline before documentation-only synchronization: `0a8c83f7776d8259f22208a66f7fc4bd52156aff`

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
Phase 18.2   NOT STARTED
Phase 18.3   NOT STARTED
Phase 18.4   NOT STARTED
Sprint 19    Local Web Control V1: SD Card File Manager / PLANNED / NOT STARTED
Sprint 20    Local Web Control V2: Playback + Volume / PLANNED / NOT STARTED
Sprint 21    Local Web Control V3: Lights / PLANNED / NOT STARTED
Sprint 22    Local Web Control V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23    Local Web Control V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24    Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

The application-structure cleanup is integrated. `main/main.c` is now a thin
entrypoint; `smart_room_app` owns product composition and
`smart_room_mcp_adapter` owns Smart Room MCP-provider adaptation. The cleanup
recorded a normal ESP-IDF build PASS but no new target HIL specific to the
structural move.

The post-Sprint-18 roadmap is approved but not started. Local Web work is
SD-card-first and is detailed in `AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.
The former Sprint 19 Wake Word plan is deferred to Sprint 24.

## Immediate next work

Do **not** rerun Phase-18.1 light acceptance merely because its source moved.
Phase 18.1 is closed unless a concrete regression is found.

Current deferred validation, in priority order when relevant:

```text
1. delayed-first-PCM / streaming regression on the current voice path
2. bounded post-structure-cleanup target smoke before a release checkpoint
3. Phase-16/16.1 endurance and resource-trend work
4. long-duration Firebase/cloud + Xiaozhi simultaneous-traffic regression
5. start Phase 18.2 only when Hải explicitly requests it
6. do not start Sprint 19-24 implementation until Hải explicitly requests it
```

## Future roadmap routing

When the current Phase 18 work is complete and Hải explicitly starts the next
roadmap item, route future sessions as follows:

```text
Sprint 19 -> Local Web Control V1: SD Card File Manager
Sprint 20 -> Local Web Control V2: Playback + Volume
Sprint 21 -> Local Web Control V3: Lights
Sprint 22 -> Local Web Control V4: Dashboard + System Status
Sprint 23 -> Local Web Control V5: Scenes + Logs + Diagnostics
Sprint 24 -> Wake Word + Advanced Voice UX
```

Do not reintroduce Wake Word as Sprint 19. The Sprint 24 voice sequence remains:
feasibility/resource audit -> continuous local capture + WakeNet/VAD -> advanced
conversation -> endurance/HIL.

Web/LCD must remain frontends over existing manager/service ownership. Web UI
must not configure/control Wi-Fi. Advanced OTA/factory-management flows remain
outside the approved Local Web scope.

## PTT / TLS — accepted smoke, endurance still deferred

Current source uses dynamic TLS buffers in PSRAM, 1 KiB outbound TLS records,
and a 20 KiB total/largest-contiguous PSRAM gate before a PTT turn starts.

Hải confirmed the repeated PTT/TLS smoke PASS during Phase-18.1 acceptance.
Therefore do not list basic PTT/TLS smoke as a Phase-18.1 blocker.

Long-duration/repeated-turn endurance and heap/stack/resource-trend checks remain
separate deferred work.

## Streaming downlink — focused regression still deferred

Current source uses a 7.68-second bounded PSRAM ingress ring and a 0.96-second
normal prefill. The 5-second prefill wait starts only after the first PCM packet,
not at `TTS_START`.

A focused delayed-first-PCM regression remains useful because the timing
semantics changed after the older accepted Phase-16.1 baseline. Do not claim it
PASS unless a newer target run explicitly records it.

## Phase 18.1 light — accepted

The user accepted the bounded matrix on 2026-09-13, including:

```text
pink 100%
green 20%
brightness 0 / 100
off
rapid color/brightness updates
solid / blink / breath / pulse / rainbow
effect-only while off -> turns on
effect-only with preserved black RGB -> visible white fallback
off + color -> rejected
off + brightness -> rejected
off + effect -> rejected
light.get_state matches applied logical state
light.get_capabilities matches the fixed contract
```

Current pulse timing is 1200 ms. The older 300 ms expectation is historical.

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

## Broader deferred integration work

- Phase-16/16.1 long-duration audio arbitration/streaming endurance.
- Repeated PTT and resource-trend checks beyond the accepted smoke run.
- Long-duration Firebase/cloud + Xiaozhi simultaneous traffic.
- Relevant visible UI/text regression only when a defect touches that path.
- Release/documentation/portfolio closure after feature and acceptance work.

## Evidence vocabulary

- `IMPLEMENTED` — source exists.
- `STATIC REVIEW COMPLETE` — source review performed.
- `BUILD VERIFIED` — an actual relevant ESP-IDF build passed.
- `HIL PASS` / `HIL ACCEPTED` — target evidence satisfies the named contract.
- `TARGETED HIL PARTIAL` — only named runtime cases have evidence.
- `PENDING` — no current evidence yet.

A build or HIL result belongs to the source/checkpoint actually tested. A later
behavior-preserving refactor may retain phase closure, but it must not be
misreported as a new hardware run.
