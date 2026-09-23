# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress multi-prompt work.
Current source, `AGENTS.md`, and canonical documentation remain authoritative.

## Active work

Branch: `feat/sprint20-playback-seek` (from local `main_including_Firebase_security`)
Merged implementation commits: `17bc390`, `2c57634`, `6f11169`, `a55f00e` (Prompt 20.1 and WAV-size documentation)
Base: `424b87451c72594834161ef8173e626cb222e156`
Phase/Sprint: Sprint 20 - Local Web Control V2: Playback + Volume
Checkpoint: Sprint 20 playback seek/polish and the WAV oversize/startup-failure
fix are uncommitted. Host tests and a serialized ESP-IDF build pass; target HIL
is pending.

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
- Local Web reserves 16 HTTPD URI handler slots for its 16 registered routes;
  this fixes `ESP_ERR_HTTPD_HANDLERS_FULL` after adding `/api/audio/seek` and
  the SD icon endpoint.
- Seek targets are validated before the worker starts: they must be on the
  owner-published 256-frame commit boundary. The Web range uses that same step,
  preventing a late prefetch `ESP_ERR_INVALID_ARG` after a user scrub.
- Five Lucide SVG icons and `LICENSE-LUCIDE.txt` were downloaded to the mounted
  SD card under `/web-icons`. The Web chrome serves only five allowlisted icon
  names through a lease-owned `image/svg+xml` endpoint.

## Validation actually run

- `local_web_server` host path, audio-policy, and icon-allowlist test: PASS.
- `smart_room_mcp_adapter` host catalog-entry test: PASS.
- `audio_manager` host playback-control, WAV-size-boundary, WAV-lease, and
  arbiter-lifecycle tests: PASS.
- `git diff --check`: PASS before commit.
- ESP-IDF 6.0.1 serialized build: PASS after icon endpoint, seek-alignment,
  and HTTPD route guards; firmware `0x2805b0`, app free 37%.
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

## Scope boundary

- Do not start Prompt 20.2 or Sprint 21 automatically.
- No push was performed; the current WAV fix has not been committed.
