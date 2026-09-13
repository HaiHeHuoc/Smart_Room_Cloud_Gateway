# Playback module

Private `audio_manager` submodule for named playback helpers and the pure
playback-control transition contract.

- **Owner:** `audio_manager`
- **Visibility:** private source; callers use public APIs from
  `audio_manager/include/`.
- **Lifecycle:** no independent task, I2S ownership, or ESP-IDF component.
- **Control policy:** the private policy validates transitions; physical
  pause/resume remains in the parent manager task at a committed 256-frame TX
  boundary. The pure policy is shared with focused host tests.
- **Promotion rule:** promote only if named playback becomes independently
  reusable outside the parent manager.
