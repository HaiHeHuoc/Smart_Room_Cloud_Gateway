# Current Working Checkpoint

Purpose: compact, overwriteable handoff for active work. Current source,
`AGENTS.md`, canonical documentation, and explicit build/HIL evidence remain
authoritative.

Updated: 2026-09-26
Active branch: `main_including_Firebase_security`
Source integration baseline: `2bf646f2f6ba4a70457f3d0a88e2a13edb4bd23f`
Baseline commit: `merge: integrate Sprint 23 Local Web V5`
Sprint-23 implementation commit: `21b2333e5d6d4e3b5484e829eeb6ba09f1658e8f`

## Current status

- Sprint 18: COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16.
- Sprint 19: SOURCE INTEGRATED / BUILD VERIFIED / TARGET HIL PARTIAL.
- Sprint 20: IMPLEMENTED / BUILD VERIFIED / TARGET HIL PENDING.
- Sprint 21: COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-24.
- Sprint 22: COMPLETE / BUILD VERIFIED / USER ACCEPTED BY HẢI ON 2026-09-25.
- Sprint 23: SOURCE INTEGRATED / BUILD VERIFIED / TARGET HIL PENDING.
- Sprint 24: PLANNED / NOT STARTED.

Sprint 23 source is now integrated into the requested branch. Do not describe
Sprint 23 as an unmerged feature branch or as not started. Target/browser HIL
has not yet been recorded, so do not claim Sprint-23 hardware acceptance.

## Sprint 23 delivered contract

- Local Web has exactly seven presentation tabs: Dashboard, Storage, Playback,
  Lights, Scenes, Logs, and Diagnostics.
- `scene_manager` owns fixed `focus`, `relax`, `night`, and `all_off`
  orchestration/results. It composes existing owner APIs and does not own
  drivers, storage, UI, or rollback state.
- Logs expose only bounded manager-owned archive metadata and sanitized records.
  Free-form detail, raw VFS paths, secrets, destructive actions, and raw-log
  download remain outside the browser contract.
- Archive retention does not delete an archive while a bounded read of that
  archive is active.
- Diagnostics expose copied bounded CPU/logging facts plus backend-owned export;
  no memory/config/credential dump or unrestricted filesystem access exists.
- HTTP remains a frontend: no LVGL, SD/FATFS, I2S/audio, NeoPixel/RMT/GPIO, or
  Wi-Fi/provisioning lifecycle ownership.
- Existing LCD Web Storage/Web Light surfaces remain; no unreachable Sprint-23
  LCD Scene/Logs/Diagnostics navigation was added.

## Recorded validation

- Local Web host/static policy suite: PASS.
- Smart Room MCP adapter host suite: PASS.
- Audio-manager host suite: PASS for retained integration coverage.
- ESP-IDF 6.0.1 serialized build: PASS for the Sprint-23 implementation state.
- Recorded firmware size at the Sprint-23 pre-HIL checkpoint: `0x294540`;
  smallest app partition free: `0x16bac0` (36%).
- Sprint-23 target/browser HIL: NOT YET RECORDED.

Build/host evidence is not target-HIL evidence.

## Remaining validation

Primary gate: run Sprint-23 target/browser HIL for seven-tab navigation, all
four fixed scenes, sanitized archive paging/recovery, Diagnostics freshness and
export, visibility-aware polling, LCD non-regression, and mixed
WAV/PTT/Xiaozhi/Web/Light/SD stress.

Deferred/non-blocking regression:
- Sprint 22 Dashboard HIL after user acceptance.
- Sprint 19 upload/interruption/mutation/remount cases beyond accepted
  capacity/status and browser-download fixes.
- Sprint 20 playback/seek/volume/PTT/Xiaozhi/SD-contention target cases.
- Sprint 21 older Light HIL; do not reopen without a concrete regression.

## Durable boundaries

- Local Web remains SD-card-first and is used only after networking exists.
- No Web Wi-Fi/provisioning control, credential erase, OTA install/update,
  factory reset, reboot, arbitrary NVS/GPIO/task/shell control.
- Preserve application -> manager/service -> driver/framework ownership.
- Never expose credentials, tokens, PoP/session material, private provider
  handles, raw pointers, or unrestricted filesystem content.

## Next action

Run Sprint-23 target/browser HIL and record only observed evidence. Sprint 24
Wake Word + Advanced Voice UX does not start automatically.
