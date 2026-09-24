# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress multi-prompt work.
Current source, `AGENTS.md`, and canonical documentation remain authoritative.

## Active work

Branch: `feat/sprint21-web-light-backend` (from local `main_including_Firebase_security`)
Merged implementation commit: `f68d6b2` (Sprint 20 seek, WAV-size/recovery,
and SD-icon delivery) fast-forwarded into local `main_including_Firebase_security`.
Base: `f68d6b2b7bcd91d6aa00cb555fa8fc3a91545b92`
Phase/Sprint: Sprint 21 - Local Web Control V3: Lights, Prompt 21.3 HIL preparation
Checkpoint: Storage UI SD synchronization, Light REST backend, Lights Web UI,
and `WEB_LIGHT` LCD status view are uncommitted. Host and ESP-IDF builds pass;
target HIL is pending.

## Delivered

- Public copied catalog seam shares the existing `/sdcard/audio` catalog cache;
  Web has no MCP-private-header dependency, filesystem scanner, or physical path.
- Local REST provides catalog, playback status, catalog-ID play, controls, and
  runtime volume 0..100. Voice-assistant arbitration remains the playback owner.
- Storage `/audio` shows `Play` only for an exact catalog-approved filename.
- Browser Playback UI has bounded status facts, one in-flight poll (750 ms
  active, 2.5 s inactive), and stops polling while the document is hidden.
- Local Web content is separated into accessible Storage and Playback tabs;
  keyboard tab selection uses Left/Right/Home/End and the active panel only.
- A committed mutation affecting `/audio` invalidates the shared catalog by
  epoch; Web clears stale tracks and retries the worker-published snapshot.
- WAV files at or above 2 GiB are rejected from FAT VFS metadata with
  `ESP_ERR_NOT_SUPPORTED` before `fopen()`/parser seek; this is not an SD I/O
  failure or a PSRAM-cache limit.
- Playback arbitration detects a terminal `source=NONE` manager snapshot after
  a WAV fails during `STARTING`, releases that request, and allows the next
  Play request to proceed.
- The Playback tab offers a keyboard-accessible scrub range with a one-shot
  generation-guarded seek on commit, resilient `mm:ss`/`hh:mm:ss` display, and
  responsive control sizing. It never overwrites an active drag from polling.
- `audio_manager` accepts a seek only for a current local WAV/retained
  recording; WAV performs the existing cooperative reader join/SD lease
  release before fresh-open at the target frame. USER-paused playback remains
  paused. Live PCM/Xiaozhi, PTT-paused, stale, and final/out-of-range targets
  reject.
- Status exports an owner-authoritative sample rate for time conversion; the
  browser does not hard-code a rate. Playback speed is deliberately deferred:
  no Web/API control changes global I2S configuration.
- Local Web reserves 18 HTTPD URI handler slots for its 18 registered routes;
  this covers `/api/audio/seek`, the SD icon endpoint, and two Light routes.
- Seek targets are validated before the worker starts: they must be on the
  owner-published 256-frame commit boundary. The Web range uses that same step,
  preventing a late prefetch `ESP_ERR_INVALID_ARG` after a user scrub.
- Five Lucide SVG icons and `LICENSE-LUCIDE.txt` were downloaded to the mounted
  SD card under `/web-icons`. The Web chrome serves only five allowlisted icon
  names through a lease-owned `image/svg+xml` endpoint.
- While the browser document is visible, it polls copied Storage status every
  two seconds. An unavailable transition clears stale files/capacity, disables
  mutation controls, and reports SD unavailable without reload; a ready
  transition reloads the directory and catalog once.
- Sprint 21.1 adds `GET /api/light/status` and atomic
  `POST /api/light/state` over `light_manager` only. The bounded optional
  query fields update one copied state and call `light_manager_set_state()`
  exactly once, then return a read-back snapshot. No Web UI Lights controls
  or NeoPixel/RMT/GPIO APIs were added.
- Sprint 21.2 adds the third accessible Storage/Playback/Lights tab with native
  power, `#RRGGBB` color, brightness, and effect controls. Color/brightness
  coalesce for 250 ms; power/effect commit immediately, and every successful
  commit reconciles through authoritative GET state. A generation guard and
  one in-flight poll prevent stale response overwrite. Light polling runs only
  for a visible Lights tab.
- `APP_GUI_SCREEN_WEB_LIGHT` is a read-only LCD view for copied power, logical
  RGB, brightness, and effect. `local_web_server` maps public
  `light_manager_state_t` snapshots into the length-one `app_gui` queue; the
  HTTP task does not call LVGL and no LCD control path was added.
- Sprint 21.3 review confirms Web and MCP both reach only the atomic
  `light_manager` product state; NeoPixel/RMT/GPIO/effect-worker ownership
  remains below that manager. A transient `light_busy` browser response now
  preserves the last copied controls and rejection feedback survives its
  read-only reconciliation instead of being overwritten as unavailable.
- The browser Light model keeps the response field `brightness_percent`, but
  its REST serializer now sends the API field `brightness`; the former direct
  serialization produced `invalid_light_request` for every brightness update.
- `light_manager` now keeps public RGB as logical sRGB and applies a fixed
  sRGB-to-linear PWM lookup only before static/color-effect output. This keeps
  Web/MCP read-back and GRB byte order unchanged while improving physical
  brightness and mixed-color perception; RGB-primary HIL at 5% confirms the
  existing channel order, but calibrated mixed-color HIL remains required.
- The single-LED product effect set now includes `strobe` (80/120 ms),
  `heartbeat` (1000 ms double pulse), and `candle` (2200 ms continuous
  flicker), alongside the original five effects. Web REST/UI, LCD state
  presentation, and Xiaozhi MCP schemas/providers share all eight names;
  multi-LED-only chase/wipe patterns remain deliberately unavailable.

## Validation actually run

- `local_web_server` host path, audio-policy, icon-allowlist, Light REST/UI
  policy, busy-state, and copied-LCD-route test: PASS.
- `smart_room_mcp_adapter` host catalog-entry test: PASS.
- `audio_manager` host playback-control, WAV-size-boundary, WAV-lease, and
  arbiter-lifecycle tests: PASS.
- `git diff --check`: PASS before Sprint 21.3 validation.
- ESP-IDF 6.0.1 serialized build: PASS after brightness-query and sRGB-PWM
  fixes plus single-LED effect expansion; firmware `0x284a00`, app free 37%.
- Rebuilt the ignored managed-component directories from the existing lock;
  the resolved versions remain pinned (including LVGL 9.4.0), and
  `dependencies.lock` has no working-tree change. This avoids the prior stale
  generated LVGL 9.6 tree and its `-Werror` deprecation failure.

## HIL required

- Browser seek while playing and user-paused; stale generation, near-end, live
  PCM/Xiaozhi, PTT-paused, and malformed target rejection. Confirm reader
  cleanup/reopen/SD lease release and that playback continues at the selected
  offset. Check PC/mobile range drag and time labels.
- Browser control during PTT/Xiaozhi, pause/resume/restart/stop, live 0/100
  volume, WAV files below 2 GiB, and SD-SPI CRC recovery during storage and
  playback contention.
- Attempt the 2.6 GB `Input_long_3.wav`: expect `ESP_ERR_NOT_SUPPORTED`, no
  `SD I/O failure reported` caused by file size, terminal request cleanup, and
  immediate acceptance of a subsequent supported WAV. Separately inject a real
  SD read failure to confirm the existing lease-drain/remount path still runs.
- Light REST: GET status before/after manager init; atomic partial/full updates,
  RGB/brightness boundaries, each effect, black/OFF effect activation, effect
  then OFF/ON, simultaneous MCP/Web commands, lock timeout, and apply failure.
- Light UI: desktop and 320 px browser layout, visible focus and tab keyboard
  navigation; color/brightness debounce, power/effect immediate updates,
  unavailable/recovery clearing, stale responses, hidden/non-Lights poll stop,
  Rainbow logical RGB, and OFF logical-state retention. Confirm `WEB_LIGHT`
  LCD facts update from Web GET/POST snapshots without an HTTP/LVGL violation.
- No dedicated host suite exists for `light_manager` or MCP Light providers;
  their public contracts were reviewed and the existing MCP host catalog suite
  plus Local Web light-policy suite remain the available automated coverage.

## Scope boundary

- Do not merge or push; this Sprint 21 working tree has not been committed.
