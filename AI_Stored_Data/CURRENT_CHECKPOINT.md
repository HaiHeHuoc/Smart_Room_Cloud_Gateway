# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress multi-prompt work.
This file is intentionally short to reduce AI read/write token cost.

Authority:
- current source, `AGENTS.md`, and canonical repository documentation remain
  higher authority;
- durable roadmap/architecture/security decisions belong in their canonical
  documents, not only here;
- overwrite superseded intermediate details instead of growing session history.

## Active work

Branch: phase/20-local-web-playback-volume
HEAD: 424b87451c72594834161ef8173e626cb222e156 (base before local changes)
Phase/Sprint: Sprint 20 — Local Web Control V2: Playback + Volume
Checkpoint: Prompt 20.1 implementation

## Implemented since last durable sync

- Added a public copied audio-catalog seam in smart_room_mcp_adapter; Web never
  includes MCP-private headers or resolves physical SD paths.
- Added runtime audio_manager playback volume 0..100 with lock-free aligned
  block-path reads, no I2S restart, and copied getter/status fact.
- Added bounded local Web audio tracks/status/play/control/volume routes and
  lightweight browser polling UI.

## Validation actually run

- local_web_server host path-policy test: PASS.
- smart_room_mcp_adapter host catalog-entry test: PASS.
- git diff --check: PASS.
- ESP-IDF 6.0.1 serialized build: PASS (firmware 0x27d500; 38% app partition free).

## Blockers / known risks

- Target HIL is required for Web/PPT/Xiaozhi arbitration, live volume response,
  progress behavior, large SD files, and current SD-SPI CRC/recovery behavior.

## Next action

- Flash this branch and execute Sprint-20 browser/board HIL; do not start
  Prompt 20.2 or Sprint 21 automatically.
