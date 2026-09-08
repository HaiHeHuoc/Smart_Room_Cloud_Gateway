# Stream module

Private `audio_manager` submodule for bounded live PCM stream buffering,
stream-tap delivery, and stream-core helpers.

- **Owner:** `audio_manager`
- **Visibility:** private implementation headers live under this module; public
  stream contracts remain in `audio_manager/include/`.
- **Lifecycle:** the parent manager remains the sole capture/playback and I2S
  owner.
- **Promotion rule:** promote only if the stream core must be reused without the
  parent audio manager lifecycle.
