# Playback module

Private `audio_manager` submodule for named playback helpers built on the
manager-owned playback path.

- **Owner:** `audio_manager`
- **Visibility:** private source; callers use public APIs from
  `audio_manager/include/`.
- **Lifecycle:** no independent task, I2S ownership, or ESP-IDF component.
- **Promotion rule:** promote only if named playback becomes independently
  reusable outside the parent manager.
