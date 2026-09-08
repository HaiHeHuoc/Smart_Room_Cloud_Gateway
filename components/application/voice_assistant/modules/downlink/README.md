# Downlink module

Private `voice_assistant` submodule for converting bounded Xiaozhi downlink audio
into the `audio_manager` streaming/playback boundary.

- **Owner:** `voice_assistant`
- **Visibility:** implementation-private; public downlink contracts remain in
  `voice_assistant/include/`.
- **Ownership rule:** it never bypasses `audio_manager` arbitration or takes I2S
  ownership directly.
- **Promotion rule:** keep private unless downlink transport becomes a stable
  reusable service independent from voice orchestration.
