# Current Working Checkpoint

Purpose: compact handoff for the Xiaozhi first-boot activation feature. Live
source, `AGENTS.md`, and recorded build/HIL evidence remain authoritative.

Updated: 2026-10-04

## Active source

- Working branch: `feature/xiaozhi-first-boot-activation`, created from
  `main` at `00ffc49`.
- Sprint 24 Wake Word + Advanced Voice UX remains **SUSPENDED PERMANENTLY**;
  this change neither revives nor depends on that work.
- The provider boundary remains `smart_room_app -> voice_assistant ->
  xiaozhi_foundation -> esp_xiaozhi` (`espressif/esp_xiaozhi` 0.1.2).

## First-boot activation

- `CONFIG_SMART_ROOM_XIAOZHI_FIRST_BOOT_ACTIVATION` is a menuconfig boolean,
  enabled by default. `sdkconfig.activation_disabled.defaults` is the explicit
  compatibility profile for the disabled behavior.
- `xiaozhi_foundation` obtains backend info once, copies activation code and
  message into bounded project-owned storage, frees the upstream info exactly
  once, and publishes nonfatal `ACTIVATION_REQUIRED` when required.
- The feature never writes an activation result to NVS: a subsequent backend
  `get_info` response remains the source of truth. Backend timeout metadata is
  displayed as information only; retries use the project-owned 10/20/40/60 s
  bounded backoff.
- `voice_assistant` schedules cooperative retry from its lifecycle task. PTT
  remains unavailable while activation is required. The UI receives only a
  copied status/model update and performs LVGL work on its owning queue/task.

## Validation performed

- Xiaozhi foundation host suite: PASS, including feature-disabled,
  activation-required, truncation, stale-clear, and retry-backoff policy.
- Voice assistant host suite: PASS, including PTT owner/lease and lifecycle
  boundaries.
- `git diff --check`: PASS.
- ESP-IDF v6.0.1 `esp32s3` serialized build: PASS with activation enabled
  (`build_activation_enabled`).
- ESP-IDF v6.0.1 `esp32s3` serialized build: PASS with activation disabled
  (`build_activation_disabled`).

## Acceptance boundary

- No new device flash, serial activation, display, or PTT hardware evidence
  was collected. HIL remains **NOT RUN**.

## Next action

Commit this focused feature, merge it to `main` only after fresh ref/ancestry
verification, then fast-forward `release/V2.0.0` from the resulting `main`
only if its history remains a direct ancestor. Do not tag, publish, or start
Sprint 24.
