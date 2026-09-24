# Next Work + Deferred HIL Backlog

Updated: 2026-09-24
Active branch: `main_including_Firebase_security`
Sprint-18 closure authority: explicit user acceptance by Hải on 2026-09-16.
Sprint 19-21 source is integrated and build verified; target HIL is partial or
pending as noted below.

## Current software state

```text
Sprint 18    COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16
Sprint 19    Local Web Control V1: SD Card File Manager / SOURCE INTEGRATED /
             BUILD VERIFIED / TARGET HIL PARTIAL
Sprint 20    Local Web Control V2: Playback + Volume / IMPLEMENTED /
             BUILD VERIFIED / TARGET HIL PENDING
Sprint 21    Local Web Control V3: Lights / IMPLEMENTED / BUILD VERIFIED /
             TARGET HIL PENDING
Sprint 22    Local Web Control V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23    Local Web Control V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24    Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

## Immediate next work

Run integrated browser/board HIL against this merge commit. This is not
authority to start Sprint 22.

1. Sprint 19: exercise 20 MiB upload, interruption/partial cleanup, mutation
   errors, and SD removal/reinsert. Capacity/status and browser-download fixes
   are already user-confirmed on the prior target revision.
2. Sprint 20: verify catalog-ID playback, pause/resume/restart/stop, volume
   0/mid/100, seek during playing and user-paused state, PTT/Xiaozhi arbitration,
   and supported WAV versus >=2 GiB rejection under SD contention.
3. Sprint 21: verify Light GET/POST state, RGB/black, brightness 0/low/mid/100,
   all thirteen effects, Wake Up/Sleep Fade completion, browser unavailable/busy recovery,
   Web/MCP last-writer behavior, and copied `WEB_LIGHT` LCD status.
4. Record serial/resource evidence: no HTTP-to-LVGL call, no stuck effect
   worker, expected SD recovery, and relevant task/memory trends.

## Deferred regression backlog — non-blocking

- Combined audio/PTT/SD timing and lease behavior.
- Long-duration Phase 16/16.1 streaming/arbitration endurance.
- Long-duration Firebase/cloud plus Xiaozhi traffic.
- Voice Recording Critical Window resource and latency measurements.
- Release-level board smoke before a future tagged release.

## Evidence vocabulary

- `IMPLEMENTED`: source exists.
- `BUILD VERIFIED`: a relevant build passed.
- `TARGET HIL PARTIAL`: only named target cases have evidence.
- `HIL PENDING`: target evidence has not been recorded for the checkpoint.
- `USER ACCEPTED`: Hải has made a project closure decision; it does not invent
  build or target-HIL evidence.
