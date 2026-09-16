# Sprint 18 Closure Record

Date: 2026-09-16
Branch: `main_including_Firebase_security`
Closure authority: explicit instruction from Hải

## Final status

```text
Sprint 18   MCP Controlled Actions                              COMPLETE / USER ACCEPTED
Phase 18.1  Light controlled actions                            COMPLETE
Phase 18.2  Audio playback/control + bounded selection          COMPLETE / USER ACCEPTED
18.2.1      Playback Control + PTT Suspension/Auto-Resume       COMPLETE
18.2.2      Bounded Playback Start + Voice SD Audio Selection   COMPLETE
Phase 18.3  Historical bounded-playback phase                   COMPLETE / ABSORBED INTO 18.2.2
Phase 18.4  cloud.push_latest                                   COMPLETE / USER ACCEPTED
Voice Recording Critical Window                                 COMPLETE / USER ACCEPTED
```

## Closure meaning

Hải explicitly instructed the project to mark the entire Sprint/Phase 18,
18.1 through 18.4, complete. This is the current project-management status and
allows the roadmap to advance to Sprint 19 when Hải explicitly requests it.

This closure does not fabricate missing technical evidence. Historical build,
host-test, HIL, resource, and environment facts remain exactly as previously
recorded. In particular:

- Phase 18.1 retains its recorded build and user-accepted HIL evidence.
- Phase 18.2 retains its implementation-checkpoint host/build evidence; older
  target-HIL matrices are now optional regression coverage rather than closure
  blockers.
- Phase 18.3 is complete by scope reconciliation because its bounded playback
  contract is already implemented in 18.2.2; no duplicate production code is
  required.
- Phase 18.4 retains the historical checkout-specific `IDF_PATH` build blocker;
  closure by user acceptance does not relabel that attempt as a build PASS.
- Voice Recording Critical Window retains host state-machine evidence and is
  closed by explicit user acceptance; no additional target-HIL run is invented.

## Delivered Sprint-18 surface

```text
light.set_state
light.get_state
light.get_capabilities

audio.control_playback { pause | resume | stop | restart }
audio.get_playback_state
audio.list_tracks
audio.play_track { track_id }
audio.play_recorded

cloud.push_latest
```

All controlled side effects continue to route through project-owned application
providers/managers. MCP does not gain arbitrary GPIO, filesystem, credential,
Wi-Fi lifecycle, reboot, OTA, shell, or unrestricted system ownership.

## Next roadmap state

```text
Sprint 19   Local Web Control V1: SD Card File Manager          PLANNED / NOT STARTED
Sprint 20   Local Web Control V2: Playback + Volume             PLANNED / NOT STARTED
Sprint 21   Local Web Control V3: Lights                        PLANNED / NOT STARTED
Sprint 22   Local Web Control V4: Dashboard + System Status     PLANNED / NOT STARTED
Sprint 23   Local Web Control V5: Scenes + Logs + Diagnostics   PLANNED / NOT STARTED
Sprint 24   Wake Word + Advanced Voice UX                       PLANNED / NOT STARTED
```

Do not start Sprint 19 automatically. It begins only when Hải explicitly asks.
The Local Web roadmap remains SD-card-first and must not add Wi-Fi configuration
or control.
