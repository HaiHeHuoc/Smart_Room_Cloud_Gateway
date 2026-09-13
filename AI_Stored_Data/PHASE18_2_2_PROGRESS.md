# Phase 18.2.2 — Bounded Playback Start + Voice SD Audio Selection

Updated: 2026-09-13

```text
Base:       main_including_Firebase_security @ f2597fd8fc60be54a66dc75dc6a7f718b9901c88
Branch:     phase/18.2.2-bounded-audio-selection
Status:     SOFTWARE IMPLEMENTED / TARGET HIL PENDING
Media root: /sdcard/audio/
```

## MCP surface

```text
audio.list_tracks {}                         -> bounded `{id,name,size_bytes}` list
audio.play_track { track_id: exact-id }      -> accepted/scheduled or bounded error
audio.play_recorded {}                       -> retained processed recording, if available
```

`audio.list_tracks` is read-only. All other actions carry no filesystem path,
FILE, SD lease, I2S, DMA, or PCM data across the MCP/provider boundary.
Its text result also enumerates each visible `name` and exact `id`, marking the
lexically first entry, so callers that consume tool text rather than structured
JSON can still answer catalog and “first song” questions correctly.
Each item also includes `size_bytes`, copied from `stat()` during the bounded
scan; it is file metadata only, not a handle or an inferred playback duration.

## Catalog and safety policy

- On-demand (not permanent) non-recursive scan of `/sdcard/audio/`.
- A scan holds one `sd_card_manager` lease only while `opendir`/`readdir`/`stat`
  are in progress; it never mounts, unmounts, or initializes SD itself.
- Maximum: 12 tracks; track ID/name: 47 bytes plus terminator; filename: 51
  bytes plus terminator.
- Eligible file is a direct bounded `.wav` entry with no separators, dot
  components, control bytes, JSON quote, or backslash. An ASCII token stem
  remains its deterministic ID; a safe display stem containing spaces or UTF-8
  receives a deterministic `track_<hash>` ID while retaining its readable
  `name`. Unknown/overlong IDs, duplicate IDs, non-WAV files, directories and
  arbitrary paths are rejected/ignored.
- Entries are retained in lexical deterministic order. If more valid files
  exist, the lexical first bounded set is exposed with `truncated=true`.
- Final WAV validation remains the existing `audio_manager` parser.

## Playback flow

```text
voice -> audio.list_tracks -> exact logical track_id -> bounded catalog lookup
      -> internal /sdcard/audio/<validated filename>.wav construction
      -> voice playback policy -> existing arbiter -> audio_manager -> I2S
```

`audio.play_track` uses the existing queueable arbiter client. During a PTT
turn with a temporary suspended source, the requested track is queued behind
Xiaozhi TTS and the turn's final action becomes STOP for the old source. Thus
the old source cannot auto-resume and overlap Track B. A direct request outside
PTT first cooperatively stops an existing local source, then queues the new
track; no playlist is created.

`audio.play_recorded` reuses the existing retained processed recording only.
It starts no capture and exposes no retained buffer. It reports
`recorded_audio_not_available` deterministically when no retained recording is
available; active TTS ordering is handled by the existing turn-final policy.

## Validation

### Static review

- No arbitrary path flows from MCP to filesystem: PASS.
- `audio_manager` remains sole I2S/DMA owner; `sd_card_manager` retains mount
  lifecycle: PASS.
- Phase 18.3/18.4 source untouched: PASS.

### Host tests

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\audio\audio_manager\test\host\run_tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\application\voice_assistant\test\host\run_tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\application\xiaozhi_foundation\test\host\run_tests.ps1
```

PASS. The Xiaozhi test includes static registration/boundary checks for the
three new tools. Existing host tests do not emulate FATFS, real SD removal,
FreeRTOS timing, I2S, or audible output.

### ESP-IDF build

ESP-IDF 6.0.1 `idf.py reconfigure` and `ninja -C build -j 1 all`: PASS.
Firmware size `0x26d4a0`; free app partition `0x192b60` (39%).

### Target HIL / system health

Not run. Required: valid/unknown tracks, empty and over-limit directory,
non-WAV/unsupported WAV, SD unavailable/removal/remount, PTT Track A -> Track
B no-auto-resume, control pause/resume/stop, retained recording availability,
and 20–50 play/stop alternations. Capture Internal/DMA/PSRAM free/min/largest,
audio and WAV-reader stack HWM, SD lease trend, CPU, and play-start latency.

## Remaining risks

- FAT directory ordering and errors are target-only behavior; the snapshot
  enforces order after scan but HIL must verify the actual card/VFS path.
- `audio.play_recorded` shares the existing direct retained-recording owner,
  whereas catalog WAV requests are arbiter-submitted; target testing must prove
  the deferred terminal handoff under the actual Xiaozhi response timing.
- Phase 18.2.1 HIL remains pending; neither its sound continuity nor 18.2.2
  selection are claimed as accepted hardware behavior.

```text
PHASE 18.2.2 READY TO CLOSE: NO
```
