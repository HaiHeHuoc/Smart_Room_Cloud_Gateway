# WAV module

Private `audio_manager` submodule for WAV parsing, validation, prefetch, and
playback support.

- **Owner:** `audio_manager`
- **Visibility:** private parser/prefetch headers are available only to the
  parent component and host tests.
- **Lifecycle:** SD leases and playback ownership remain controlled by the
  parent audio manager and `sd_card_manager` contracts.
- **Promotion rule:** promote only if WAV parsing/playback becomes a separately
  reusable service with a stable standalone API.
