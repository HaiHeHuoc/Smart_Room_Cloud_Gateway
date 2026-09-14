# Arbitration module

Private `audio_manager` submodule for capture/playback ownership arbitration and
busy-policy decisions.

- **Owner:** `audio_manager`
- **Visibility:** private implementation; external components use the public
  arbitration headers exposed from `audio_manager/include/`.
- **Lifecycle:** owned by the parent audio manager; this directory does not
  register an ESP-IDF component or create an independent service lifecycle.
- **Suspension:** an arbiter-owned WAV remains current while the manager
  snapshot is `PAUSED`; the bounded pending request is not promoted until the
  current request stops, completes, or fails.
- **Manager hand-off:** an accepted request remains arbiter-owned while its
  manager command is dispatched. The dispatch state is reconciled before the
  slot can be retried, cancelled, or promoted, preventing an unowned manager
  playback. A closed PCM ingress ring is not terminal until `audio_manager`
  no longer reports PCM source ownership.
- **Result meaning:** arbiter acceptance/queueing is a scheduling result, not
  confirmation that a WAV has opened or that audible speaker output occurred.
- **Promotion rule:** split into a standalone component only if arbitration is
  reused independently from `audio_manager` in another product.
