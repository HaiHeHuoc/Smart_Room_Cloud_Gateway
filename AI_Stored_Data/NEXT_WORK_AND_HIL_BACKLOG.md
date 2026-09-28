# Next Work + Deferred HIL Backlog

Updated: 2026-09-28
Active release-hardening branch: `release/V2.0.0`
Source integration branch: `main_including_Firebase_security`
Exact V2 baseline: `44e6feb23f3358917171b6b326b56fdec8ae7ff3` (PTT uplink robustness merge)
Sprint-18 closure authority: explicit user acceptance by Hải on 2026-09-16.
Sprint 19-23 source is integrated/build verified as noted below. Sprint 21 and
Sprint 22 are closed by user acceptance. Sprint-23 target/browser HIL is the
current acceptance gate; Sprint-22 HIL is non-blocking regression coverage.

## Current software state

```text
Sprint 18    COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16
Sprint 19    Local Web Control V1: SD Card File Manager / SOURCE INTEGRATED /
             BUILD VERIFIED / TARGET HIL PARTIAL
Sprint 20    Local Web Control V2: Playback + Volume / IMPLEMENTED /
             BUILD VERIFIED / TARGET HIL PENDING
Sprint 21    Local Web Control V3: Lights / COMPLETE / USER ACCEPTED BY HẢI ON
             2026-09-24; older HIL is deferred regression coverage
Sprint 22    Local Web Control V4: Dashboard + System Status / COMPLETE /
             BUILD VERIFIED / USER ACCEPTED BY Hai ON 2026-09-25
Sprint 23    Local Web Control V5: Scenes + Logs + Diagnostics / SOURCE INTEGRATED /
             BUILD VERIFIED / USER ACCEPTED BY HAI ON 2026-09-26 /
             TARGET/BROWSER HIL EVIDENCE PENDING
Sprint 24    Wake Word + Advanced Voice UX / SUSPENDED FOR V2 RELEASE
```

## Immediate next work

Prompts 2-5 completed the V2 source/runtime-policy audit, clean ESP-IDF build,
and all self-contained host suites. V2-R08/P1 is resolved in source: bootstrap
rollback is transactional, cleanup timeout fails closed without deinitializing
dependencies of a possibly live owner, and automatic retry is allowed only
after complete rollback. The V2 source gate is ready for Prompt 6 HIL; do not
begin it automatically and do not begin Sprint 24. Sprint-23 target/browser HIL
and Sprint-22 HIL remain release-validation/regression work; neither changes
Sprint 24 status.

1. Sprint 22: verify Dashboard endpoint/card partial failures and recovery;
   active-tab and hidden-document polling; return during an in-flight request;
   desktop/tablet/320px layout; four-tab keyboard navigation; and quiet serial
   behavior while SD/audio/light/Wi-Fi/cloud state changes externally.
2. Sprint 19: exercise 20 MiB upload, interruption/partial cleanup, mutation
   errors, and SD removal/reinsert. Capacity/status and browser-download fixes
   are already user-confirmed on the prior target revision.
3. Sprint 20: verify catalog-ID playback, pause/resume/restart/stop, volume
   0/mid/100, seek during playing and user-paused state, PTT/Xiaozhi arbitration,
   and supported WAV versus >=2 GiB rejection under SD contention.
4. Sprint 21 optional regression: verify Light GET/POST state, RGB/black,
   brightness 0/low/mid/100, all thirteen effects, Wake Up/Sleep Fade
   completion, browser unavailable/busy recovery, Web/MCP last-writer behavior,
   and copied `WEB_LIGHT` LCD status. This does not reopen Sprint 21.
5. Record serial/resource evidence: no HTTP-to-LVGL call, no stuck effect
   worker, expected SD recovery, and relevant task/memory trends.

## Sprint 22 target HIL matrix

- [ ] Browser/API: open Dashboard first, confirm HTTP 200/no-store snapshot and
  `ready|attention|unavailable`; force one unavailable manager and confirm the
  remaining cards still render.
- [ ] Sensor/Time: validate current, stale, failed/no-valid, and resynchronized
  states; no `-1`, NaN, Infinity, or false “just now” age is displayed.
- [ ] Storage: remove/reinsert SD while Dashboard is open; capacity clears while
  unavailable, recovers after READY, and Dashboard polling does not disrupt a
  Storage operation or managed lease drain.
- [ ] Audio: observe Dashboard while WAV is playing/paused and while Xiaozhi/PTT
  owns audio; no transport command, arbitration change, I2S error, or polling
  side effect is introduced.
- [ ] Lights: change each of the thirteen effects from Lights/MCP; Dashboard
  eventually reflects authoritative logical RGB/power/brightness/effect and
  never claims instantaneous physical Rainbow output.
- [ ] Network/Cloud: observe disconnect/reconnect, IPv4/RSSI validity, cloud
  retry/online, and time sync transitions; Dashboard remains read-only.
- [ ] Polling/UI: verify desktop, tablet, and 320px widths; four-tab arrow and
  Home/End navigation; hidden-tab pause; return-to-Dashboard refresh during an
  outstanding request; failed request retains the prior snapshot then recovers.
- [ ] Stability: inspect normal serial output for HTTP/manager errors only when
  induced, with no per-poll log spam, repeated HTTPD faults, heap decline, stack
  warning, or visible regression in Storage, Playback, or Lights.

## Sprint 23 target/browser HIL matrix

- [ ] Navigation: seven tabs work on desktop/tablet/320 px; tab strip scrolls
  without page overflow; ArrowLeft/Right and Home/End preserve selection.
- [ ] Scenes: apply Focus, Relax, Night, and All Off; verify honest partial or
  busy results, direct Light/Audio overrides, and no Xiaozhi/PTT interruption.
- [ ] Logs: load closed archive metadata and pages; verify EOF, malformed or
  partial lines, no raw detail/path/secret exposure, and SD remove/recover.
- [ ] Diagnostics: verify first CPU sample, freshness age, 500 ms peak,
  recording-critical deferral, and bounded export with no secret/path/raw log.
- [ ] Polling: active-only cadence, hidden-document stop, visible return with
  one refresh, and no overlapping/stale-response UI overwrite.
- [ ] LCD: existing Web Storage and Web Light remain usable; no Logs LCD view
  and no HTTP callback reaches LVGL.
- [ ] Stress: mix WAV/PTT/Xiaozhi, Web polling, Light/Scene, and SD access for
  10-15 minutes; observe task count/heap trends, HTTPD faults, SD leases,
  audio timeouts, and logger drops without inventing hard thresholds.

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
