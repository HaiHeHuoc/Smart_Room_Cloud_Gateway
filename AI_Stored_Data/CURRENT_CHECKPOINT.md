# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress work. Current source,
`AGENTS.md`, and canonical documentation remain authoritative.

## Active work

Branch: `feat/sprint21-extended-light-effects` (from
`main_including_Firebase_security` at `3813945`)
Integration: Sprint 21 extended-effects implementation is uncommitted and has
not been merged or pushed.
Phase/Sprint: Sprint 21 — Local Web Control V3: Lights
Checkpoint: source is implemented and build verified; target HIL remains
pending except the user-confirmed color-channel check and prior Sprint 19
capacity/download checks.

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

## Validation actually run

- `components/application/local_web_server/test/host/run_tests.ps1`: PASS.
- `components/application/smart_room_mcp_adapter/test/host/run_tests.ps1`:
  PASS.
- `components/audio/audio_manager/test/host/run_tests.ps1`: PASS after the
  Sprint 21 effect expansion.
- `git diff --check`: PASS after the final effect-scope validation change.
- ESP-IDF 6.0.1 serialized build: PASS; firmware `0x285380`, app free 37%.

## HIL still required

- Sprint 19: upload/interruption cleanup, mutations, and SD removal/remount.
  Capacity/status and browser download are user-confirmed on the prior target
  revision.
- Sprint 20: supported WAV playback/seek/volume with PTT/Xiaozhi arbitration,
  SD contention, and the >=2 GiB rejection/recovery path.
- Sprint 21: Light REST/UI/LCD updates, black/RGB/brightness boundaries, all
  thirteen effects, Wake Up/Sleep Fade completion semantics, Web/MCP concurrency, unavailable/busy
  recovery, and resource/effect-worker trends.

## Scope boundary

- Do not begin Sprint 22 automatically.
- The local merge does not imply remote push or target-HIL acceptance.
