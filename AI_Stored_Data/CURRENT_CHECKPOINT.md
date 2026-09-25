# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress work. Current source,
`AGENTS.md`, and canonical documentation remain authoritative.

## Active work

Branch: `feat/sprint22-dashboard-backend` (from
`main_including_Firebase_security` at `b4431ff`)
Integration: Sprint 22.1 implementation is uncommitted and has not been merged
or pushed.
Phase/Sprint: Sprint 22.3 — Integration Hardening, Documentation, HIL Preparation
Checkpoint: Sprint 22 backend/UI and final hardening are uncommitted on this
branch. Host checks and the serialized build pass; target/browser HIL is ready
to run but has not been claimed.

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

## Validation actually run

- `components/application/local_web_server/test/host/run_tests.ps1`: PASS.
- `components/application/smart_room_mcp_adapter/test/host/run_tests.ps1`:
  PASS.
- `components/audio/audio_manager/test/host/run_tests.ps1`: PASS after the
  Sprint 21 effect expansion.
- `git diff --check`: PASS after Sprint 22.3 hardening/documentation.
- ESP-IDF 6.0.1 serialized build: PASS; firmware `0x289b00`, app free 37%.

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

## Scope boundary

- Do not begin Sprint 23. No performance/resource dashboard fields are
  available until a future public copied status API is deliberately introduced.
- No merge or remote push is authorized by this checkpoint.
