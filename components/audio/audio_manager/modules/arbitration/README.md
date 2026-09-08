# Arbitration module

Private `audio_manager` submodule for capture/playback ownership arbitration and
busy-policy decisions.

- **Owner:** `audio_manager`
- **Visibility:** private implementation; external components use the public
  arbitration headers exposed from `audio_manager/include/`.
- **Lifecycle:** owned by the parent audio manager; this directory does not
  register an ESP-IDF component or create an independent service lifecycle.
- **Promotion rule:** split into a standalone component only if arbitration is
  reused independently from `audio_manager` in another product.
