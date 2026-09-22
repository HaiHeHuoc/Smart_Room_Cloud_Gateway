# Current Working Checkpoint

Purpose: compact, overwriteable handoff for in-progress multi-prompt work.
Current source, `AGENTS.md`, and canonical documentation remain authoritative.

## Active work

Branch: `phase/20-local-web-tabs`
Implementation commits: `17bc390`, `2c57634` (Prompt 20.1 delivery)
Base: `424b87451c72594834161ef8173e626cb222e156`
Phase/Sprint: Sprint 20 - Local Web Control V2: Playback + Volume
Checkpoint: Prompt 20.1 implemented; target HIL pending.

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

## Validation actually run

- `local_web_server` host path and audio-policy test: PASS.
- `smart_room_mcp_adapter` host catalog-entry test: PASS.
- `audio_manager` host playback-control and WAV-lease tests: PASS.
- `git diff --check`: PASS before commit.
- ESP-IDF 6.0.1 serialized build: PASS; firmware `0x27ea60`, app free 38%.

## HIL required

- Browser control during PTT/Xiaozhi, pause/resume/restart/stop, live 0/100
  volume, progress behavior, PC/mobile layout, large WAV, and SD-SPI CRC
  recovery during storage and playback contention.

## Scope boundary

- Do not start Prompt 20.2 or Sprint 21 automatically.
- No push or merge was performed.
