# Phase 18.2.2 — Bounded Playback Start + Voice SD Audio Selection

Updated: 2026-09-14

```text
Base:       main_including_Firebase_security @ f2597fd8fc60be54a66dc75dc6a7f718b9901c88
Branch:     phase/18.2.2-bounded-audio-selection
Status:     SOFTWARE IMPLEMENTED / TARGET HIL PENDING
Media root: /sdcard/audio/
```

## MCP surface

```text
audio.list_tracks {}                         -> published bounded catalog snapshot
audio.play_track { track_id: exact-id }      -> accepted/scheduled or bounded error
audio.play_recorded {}                       -> retained processed recording, if available
```

`audio.list_tracks` is read-only. All other actions carry no filesystem path,
FILE, SD lease, I2S, DMA, or PCM data across the MCP/provider boundary. Its
structured result is only a compact availability/truncation/count summary; its
bounded text result enumerates each visible `name`, exact `id`, and
`size_bytes`, marking the lexically first entry. `size_bytes` is file metadata,
not a handle or inferred playback duration. `accepted` and `scheduled` from
either play tool mean an arbiter request exists only; their
`playback_confirmed` field remains `false` and must not be presented as audible
speaker output.

## Catalog and safety policy

- A persistent low-priority catalog worker performs the bounded non-recursive
  `/sdcard/audio/` scan. It retries every two seconds while unavailable and is
  also woken by a catalog request; this avoids use of the SD manager's single
  availability callback.
- Only that worker holds an `sd_card_manager` lease while
  `opendir`/`readdir`/`stat` are in progress. MCP/WebSocket callbacks use a
  zero-wait cache copy/look-up and never mount, unmount, initialize, or scan
  the SD card.
- The persistent catalog snapshot, worker stack, and 1.2 KiB MCP list staging
  buffer are allocated in PSRAM to preserve Internal RAM headroom for TLS,
  I2S, and transport paths. A cache snapshot is boundedly stale between scans;
  it is not an instantaneous view of the card.
- Maximum: 12 tracks; track ID/name: 47 bytes plus terminator; filename: 51
  bytes plus terminator.
- Eligible file is a direct bounded `.wav` entry with no separators, dot
  components, control bytes, JSON quote, or backslash. An ASCII token stem
  remains its deterministic ID; a safe display stem containing spaces or UTF-8
  receives a deterministic `track_<hash>` ID while retaining its readable
  `name`. Unknown/overlong IDs, duplicate IDs, non-WAV files, directories and
  arbitrary paths are rejected/ignored.
- Entries are retained in lexical filename order. Logical IDs are assigned
  after that ordering and collision-suffixed deterministically, so exactly one
  retained filename maps to each bounded token-safe ID. If more valid files
  exist, the lexical first bounded set is exposed with `truncated=true`.
- VFS media errors from `readdir`, `stat`, or `closedir` invalidate the whole
  snapshot and are reported to `sd_card_manager`; a partial catalog is not
  published after a media error.
- Final WAV validation remains the existing `audio_manager` parser.

## Playback flow

```text
catalog worker -> published snapshot -> audio.list_tracks
voice -> exact logical track_id -> bounded cache lookup
      -> internal /sdcard/audio/<validated filename>.wav construction
      -> voice playback policy -> existing arbiter -> audio_manager -> I2S
```

`audio.play_track` uses the existing queueable arbiter client. During a PTT
turn with a temporary suspended source, the requested track is queued behind
Xiaozhi TTS and the turn's final action becomes STOP for the old source. Thus
the old source cannot auto-resume and overlap Track B. A direct request outside
PTT first cooperatively stops an existing local source, then queues the new
track; no playlist is created.

The arbiter keeps a request owned through manager-command dispatch and does not
terminalize a PCM request merely because its ingress ring closed. GPIO38
interrupt, callback, timeout, and queue items are additionally scoped by a
private response epoch, preventing stale locally queued downlink work from
affecting the next response on the same WebSocket session.

The current response epoch is reserved before MANUAL `stop-listening` is sent.
Only that path opens response delivery immediately before the synchronous
transmit, preventing the first TTS/Opus callback from racing the send return;
all cleanup-only paths keep the gate closed. Channel cleanup, abort, and a
transport fence close the gate again.

A local response abort also signals the provider while the current audio
session ID is valid and marks that client generation as requiring a hard
transport fence. Before another PTT capture, `voice_assistant` reserves a
replacement generation, stops the old WebSocket task, drains old global events
through a private FIFO sentinel, and waits for a fresh READY connection. Old
TCP/TLS/WebSocket frames therefore cannot enter the next response. Any
stop/start/drain failure remains fail-closed: PTT is not authorized to capture.

`audio.play_recorded` reuses the existing retained processed recording only.
It starts no capture and exposes no retained buffer. It reports
`recorded_audio_not_available` deterministically when no retained recording is
available; active TTS ordering is handled by the existing turn-final policy.

## Validation

### Static review

- No arbitrary path flows from MCP to filesystem: PASS.
- `audio_manager` remains sole I2S/DMA owner; `sd_card_manager` retains mount
  lifecycle: PASS.
- Phase 18.3 historical playback scope is fully covered by 18.2.2: PASS.
- Phase 18.4 is implemented separately on its dedicated branch; it is not
  evidence for Phase 18.2 target acceptance.

### Host tests

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\audio\audio_manager\test\host\run_tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\application\voice_assistant\test\host\run_tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\application\xiaozhi_foundation\test\host\run_tests.ps1
```

PASS after the 2026-09-14 stability hardening. The Xiaozhi test includes static
registration/boundary checks for the new tools, PSRAM/cache path, result
contract, and the transport stop/FIFO-drain/restart fence. Voice/audio tests
cover the pure turn-policy state machine plus source guards for terminal,
interrupt, PCM hand-off, rapid-resume, and PTT's fail-closed fence admission.
They do not emulate FATFS, real SD removal, FreeRTOS interleavings, I2S, TLS,
or audible output.

### ESP-IDF build

ESP-IDF 6.0.1 `ninja -C build -j 1 all`: PASS after hardening.
Firmware size `0x26fb90`; free app partition `0x190470` (39%).
`ninja -C build size`: DIRAM `167376 / 341760` bytes (48.97%); moving the MCP
list staging buffer to PSRAM reduced `xiaozhi_foundation` DIRAM BSS from 1453
to 197 bytes in the component map. This is static-link evidence only; target
heap minima and task high-water marks remain required HIL evidence.

### Target HIL / system health

Not run. Required: valid/unknown tracks, empty and over-limit directory,
non-WAV/unsupported WAV, SD unavailable at boot then remount/recovery, media
error invalidation, and list/play requests while the worker scans. Run 20–50
GPIO38 response interruptions and play/stop alternations, including rapid
pause/resume and a catalog request racing PTT admission. For each interruption,
hold PTT through the expected `TRANSPORT_FENCE_BEGIN` and
`TRANSPORT_FENCE_READY` logs; verify that capture is never authorized before
fresh READY, and that a fence error keeps it blocked. Confirm no stale
`PCM_STREAM_REJECTED`/response abort or old speech appears after a completed
cancellation; confirm an accepted track request either reaches the WAV-start
log or reports a later manager failure. Capture Internal/DMA/PSRAM
free/min/largest, catalog worker/downlink/arbiter/WAV-reader stack HWM, SD
lease trend, CPU, and play-start latency.

## Remaining risks

- FAT directory ordering and errors are target-only behavior; the snapshot
  enforces order after scan but HIL must verify the actual card/VFS path.
- The catalog is a bounded cache. A changed card can remain represented by the
  prior snapshot until the next scan completes.
- The Xiaozhi provider still exposes no server response ID, but every locally
  aborted response now requires a new TCP/TLS/WebSocket transport generation
  before PTT recapture. Target evidence must still confirm the fence logs and
  absence of stale playback under real network timing.
- `audio.play_recorded` shares the existing direct retained-recording owner,
  whereas catalog WAV requests are arbiter-submitted; target testing must prove
  the deferred terminal handoff under the actual Xiaozhi response timing.
- Phase 18.2.1 HIL remains pending; neither its sound continuity nor 18.2.2
  selection are claimed as accepted hardware behavior.

```text
PHASE 18.2.2 READY TO CLOSE: NO
```
