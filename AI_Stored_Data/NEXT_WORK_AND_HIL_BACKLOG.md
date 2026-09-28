# Next Work + Deferred HIL Backlog

Updated: 2026-09-28
Active branch: `main_including_Firebase_security`
Source integration baseline: `44e6feb23f3358917171b6b326b56fdec8ae7ff3` (PTT uplink robustness merge)
Sprint-18 closure authority: explicit user acceptance by Hải on 2026-09-16.
Sprint 19-23 source is integrated/build verified as noted below. Sprint 21,
Sprint 22, and Sprint 23 are closed by user acceptance. Sprint-23 target/browser
HIL and Sprint-22 HIL are non-blocking regression coverage. The current focused
validation gate is the integrated Xiaozhi PTT uplink robustness fix.

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
Sprint 23    Local Web Control V5: Scenes + Logs + Diagnostics / COMPLETE /
             USER ACCEPTED BY HẢI ON 2026-09-26
Sprint 24    Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

## Immediate next work

Primary gate: validate the PTT uplink robustness merge on target hardware.

1. Run several normal GPIO38 PTT turns, including speech immediately after
   press and continuous speech for 5-10 seconds.
2. Capture the `VOICE_UPLINK` summary and check `queue_drops`, `stale_drops`,
   `queue_peak=<N>/16`, `max_encode_us`, `max_send_us`,
   `capture_to_first_pcm_ms`, and `capture_to_first_opus_ms`.
3. Check `audio_manager` RX overflow/timeout deltas. Nominal evidence is zero
   queue drops, zero unexpected stale drops, and zero RX overflow/timeout delta.
4. Repeat with Local Web open; optionally attempt Storage upload/download and
   confirm the recording-critical policy rejects/defers the heavy transfer.
5. If queue depth still reaches 16/16 or drops occur while RX remains clean,
   investigate a second bounded Opus packet queue between encode and blocking
   send. Do not enlarge DMA descriptors or add another sender task first.

After that, older Sprint-19/20/22/23 matrices remain useful regression work but
do not reopen already accepted Sprints 21-23 without a concrete regression.
Sprint 24 remains PLANNED / NOT STARTED until Hải explicitly starts it.

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

## Sprint 23 optional target/browser regression matrix

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

- Sprint 23 seven-tab/Scenes/Logs/Diagnostics browser matrix after its
  2026-09-26 user acceptance.
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
