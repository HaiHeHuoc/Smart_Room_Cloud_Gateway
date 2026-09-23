# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress multi-prompt work.
Current source, `AGENTS.md`, and canonical documentation remain authoritative.

## Active work

Branch: `main_including_Firebase_security`
Implementation/docs baseline inspected: `a55f00ef94de94f0150314aef5d92770b29d058c`
Phase/Sprint: Sprint 20 - Local Web Control V2: Playback + Volume
Checkpoint: Prompt 20.1 is integrated on the main integration branch; target HIL is pending.
Sprint 19 storage HIL remains partial rather than fully closed.

## Delivered

- Local REST now exposes `/api/audio/status`, `/api/audio/tracks`,
  `/api/audio/play`, `/api/audio/control`, and `/api/audio/volume`.
- Public copied catalog APIs share the bounded `/sdcard/audio` catalog cache;
  Web receives copied logical IDs/metadata only and no physical path, SD handle,
  MCP-private header, or independent filesystem scanner.
- Catalog-approved playback routes through the existing voice-assistant/audio
  ownership path. Web never takes I2S ownership.
- `audio_manager` exposes runtime playback volume in the inclusive 0..100
  range while retaining manager-owned playback/arbitration.
- Storage `/audio` shows Play only for an exact catalog-approved filename.
- Browser Playback UI has bounded status facts, one in-flight poll (750 ms when
  active, 2.5 s when inactive), and polling stops while the document is hidden.
- Local Web content is separated into accessible Storage and Playback tabs;
  keyboard tab selection supports Left/Right/Home/End and only the active panel
  is exposed.
- A committed storage mutation under `/audio` invalidates the shared catalog
  by epoch; Web clears stale tracks and retries the worker-published snapshot.

## Current playback constraint

- WAV playback currently supports files smaller than 2 GiB, up to
  `2,147,483,647` bytes.
- This is a current FAT VFS/C stdio seek/tell limitation in the playback path
  using `FILE *`, `fseek()`, and `ftell()`; it is not an SD-capacity or
  PSRAM-cache-size limit.
- FAT32 may still list/download 2-4 GiB files through storage APIs, but those
  files are not supported by the current WAV playback reader.

## Recorded validation from Prompt 20.1

These results were recorded by the implementation checkpoint; this AI-memory
sync did not rerun them:

- `local_web_server` host path and audio-policy test: PASS.
- `smart_room_mcp_adapter` host catalog-entry test: PASS.
- `audio_manager` host playback-control and WAV-lease tests: PASS.
- `git diff --check`: PASS before the implementation commit.
- ESP-IDF 6.0.1 serialized build: PASS; firmware `0x27eee0`, app free 38%.

## HIL still required

- Browser playback/control during PTT/Xiaozhi arbitration.
- Pause/resume/restart/stop behavior and progress reporting.
- Runtime volume boundaries 0 and 100.
- PC/mobile Storage + Playback tab behavior.
- WAV playback for valid files below 2 GiB and rejection/failure behavior for
  files at or above the current limit.
- SD-SPI CRC/recovery behavior under storage/playback contention.
- Remaining Sprint 19 storage HIL: upload through the approved 20 MiB limit,
  interruption/partial-file cleanup, mutations, and SD removal/remount/recovery.

## Scope boundary

- Do not mark Sprint 20 complete from Prompt 20.1 alone.
- Do not start Prompt 20.2 or Sprint 21 automatically.
- This synchronization updates `AI_Stored_Data` only; it does not change
  firmware/source implementation or claim new HIL/build evidence.
