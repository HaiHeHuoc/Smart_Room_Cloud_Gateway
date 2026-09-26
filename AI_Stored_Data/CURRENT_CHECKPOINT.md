# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress work. Current source,
`AGENTS.md`, and canonical documentation remain authoritative.

## Active work

Branch: `feat/sprint23-scene-backend` (from
`main_including_Firebase_security` at `97935d3`)
Integration: Sprint 22 is COMPLETE / BUILD VERIFIED / USER ACCEPTED BY Hai on
2026-09-25. No Sprint-23.1 commit, merge, or push has occurred.
Phase/Sprint: Sprint 23.2 — Safe Logs Backend
Checkpoint: Sprint 23.1 Scenes remains present. Sprint 23.2 adds bounded,
owner-routed archive metadata/read APIs and sanitized Local Web REST. Host test
passes; ESP-IDF build and target HIL are not claimed for this code state.

### Sprint 23.4 correction (supersedes the stale 23.2 heading above)

Phase/Sprint: Sprint 23.4 - Web UI + LCD Read-Only Integration.
Sprint 23.1 Scenes, 23.2 Safe Logs, and 23.3 Diagnostics backend are present
in this uncommitted local worktree. Sprint 23.4 adds seven-tab Web
presentation over those contracts. Host/build validation is recorded only after
this checkpoint's commands complete; target HIL is not claimed.

### Sprint 23.5 correction (supersedes the status headings above)

Sprint 23.1 Scenes, 23.2 Safe Logs, 23.3 Diagnostics, and 23.4 seven-tab Web
presentation are implemented. 23.5 preserves their owner boundaries, fixes the
confirmed one-shot chunked-response completion defect, and prepares target HIL.
Build/host validation is recorded; target acceptance, merge, and push are not
claimed.

## Delivered

- Local Web provides Storage, Playback, and accessible Lights tabs. Browser
  polling stops while hidden/inactive and avoids stale response overwrite.
- SD removal/ready transitions clear stale storage state and reload once after
  recovery. Audio catalog, playback, volume, and frame-aligned WAV seek remain
  owner-routed through the existing audio/voice path.
- WAV files at or above 2 GiB are rejected before open/seek with
  `ESP_ERR_NOT_SUPPORTED`; this does not invoke SD I/O recovery merely due to
  file size. Failed WAV startup releases terminal arbitration ownership.
- Light REST exposes copied state through `GET /api/light/status` and atomic
  partial state updates through `POST /api/light/state`. The browser maps its
  `brightness_percent` model field to REST `brightness` before POST.
- Web and MCP reach only `light_manager`; neither Web nor MCP owns NeoPixel,
  RMT, GPIO, effect workers, or LVGL. `app_gui` receives copied, length-one
  `WEB_LIGHT` status snapshots and the LCD remains read-only.
- Logical public RGB remains sRGB. `light_manager` converts it to linear PWM at
  the driver boundary, preserving RGB/GRB byte-order configuration and API
  read-back. User photos at 5% confirmed the configured RGB channel order.
- Single-LED effects are `solid`, `blink`, `breath`, `pulse`, `rainbow`,
  `strobe`, `heartbeat`, `candle`, `sos`, `lightning`, `wake_up`,
  `sleep_fade`, and `notification`. Chase/wipe patterns remain unavailable
  because they are not meaningful on the current one-LED board. Wake Up ends
  at stable configured output, Sleep Fade latches dark until a newer command,
  and Notification repeats its bounded double-flash pattern to avoid a second
  product-state owner.
- Sprint 21 is user-accepted. Its older HIL checklist remains non-blocking
  regression material and must not reopen that sprint.
- `GET /api/dashboard/status` streams one bounded, no-store operational
  snapshot from public copied status APIs: sensor, storage, audio/playback,
  light, cloud, network, time, and monotonic uptime. Each section degrades
  independently with deterministic `ready|attention|unavailable` overall state.
- No Dashboard write route, Wi-Fi/provisioning control, direct driver/LVGL
  access, MCP-private dependency, secret/config exposure, or
  performance-monitor log scraping was added. Resource metrics are deferred
  because `performance_monitor` has no copied public status snapshot.
- Dashboard is the first/default accessible Web tab and renders seven compact,
  safe-DOM cards from the existing no-store Dashboard snapshot. It shows only
  copied operational facts and has no controls.
- Dashboard polling is one in-flight request at most, every two seconds only
  while its tab and document are active. Tab/visibility changes stop pending
  scheduling, returning refreshes the active tab immediately, and a generation
  guard rejects stale responses. Fetch failure retains the last accepted cards.
- Dashboard rejects malformed optional numeric JSON fields, does not calculate
  capacity from invalid bounds, and treats a future sensor timestamp as an
  unknown age. A return during an in-flight request queues one immediate
  refresh after completion without overlapping requests.
- Scenes loads its backend catalog once, applies only backend-owned IDs, guards
  duplicate requests, and renders historical last-applied plus aggregate and
  per-domain outcomes without claiming an active-state match.
- Logs renders only `log_manager` public archive metadata and bounded sanitized
  records. Archive scans are entry/manual only; record pages replace rather
  than grow the DOM. No raw detail, VFS path, destructive action, live tail, or
  raw log download is exposed.
- Diagnostics renders only the current copied CPU/logging fields and the
  backend-owned export. Its five-second active/visible-only poll has one request
  in flight and shows unavailable performance samples honestly. `age_ms` now
  reports the completed monitor sample age rather than a placeholder zero.
- Existing `app_gui` has no clean product navigation path to additional Local
  Web subviews. Sprint 23.4 therefore adds no unreachable LCD Scene/Diagnostics
  screens and does not let HTTP route or call LVGL.
- Pre-23.5 review corrected mixed Scene outcome aggregation so a successful
  Light/Audio domain is never hidden by another domain's busy/unavailable
  result. It also fences archive traversal by owned directory/file counts and
  withholds a Logs forward cursor if a malformed line exhausts the scan budget.

## Validation actually run

- `components/application/local_web_server/test/host/run_tests.ps1`: PASS.
- `components/application/smart_room_mcp_adapter/test/host/run_tests.ps1`:
  PASS.
- `components/audio/audio_manager/test/host/run_tests.ps1`: PASS after the
  Sprint 21 effect expansion.
- `git diff --check`: PASS after Sprint 22.3 hardening/documentation.
- ESP-IDF 6.0.1 serialized build: PASS; firmware `0x289b00`, app free 37%.
- Sprint 23.4 `local_web_server` host/static suite: PASS.
- Sprint 23.4 `git diff --check`: PASS.
- Sprint 23.4 pre-23.5 review ESP-IDF 6.0.1 serialized build: PASS; firmware
  `0x294540`, smallest app partition free `0x16bac0` (36%).

## HIL still required

- Sprint 19: upload/interruption cleanup, mutations, and SD removal/remount.
  Capacity/status and browser download are user-confirmed on the prior target
  revision.
- Sprint 20: supported WAV playback/seek/volume with PTT/Xiaozhi arbitration,
  SD contention, and the >=2 GiB rejection/recovery path.
- Sprint 21 is COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-24. Its older Light
  HIL matrix is optional deferred regression coverage, not a Sprint-21 blocker.
- Sprint 22 target/browser HIL: Dashboard response/partial-manager degradation,
  tab/visibility polling race, 320px/tablet/desktop layout, existing-tab
  regression, and resource/serial stability observations.

## Sprint 23.1 boundary

- `scene_manager` owns only the fixed `focus`, `relax`, `night`, and `all_off`
  orchestration/result contract. It has no driver, task, queue, UI, storage, or
  persistence ownership; partial owner results have no rollback.
- The Web presentation is implemented; custom/persisted/scheduled scenes remain
  out of scope and there is no LCD log browser.
- No merge or remote push is authorized by this checkpoint.

## Sprint 23.2 boundary

- Browser-visible log records contain only timestamp/time validity, uptime,
  level, tag, and event. Free-form detail is never serialized.
- Closed manager-owned archive access is bounded to 12 files, six records per
  page, and 4096 scanned bytes. No raw download or log mutation exists.
- Sprint 23.3 Diagnostics and Sprint 23.4 UI are implemented; target HIL is
  still not claimed.

## Sprint 23.4 boundary

- Local Web navigation has exactly Dashboard, Storage, Playback, Lights,
  Scenes, Logs, and Diagnostics. Hidden/inactive tabs stop their timers and
  keyboard tab activation preserves roving tabindex and scrolls overflow tabs
  into view.
- Diagnostics remains a periodic copied snapshot/export surface, not a
  measurement, control, raw-log, credential, path, or memory-dump surface.
- No merge or remote push is authorized by this checkpoint.
