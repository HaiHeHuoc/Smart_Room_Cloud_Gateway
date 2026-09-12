# Stream module

Private `audio_manager` submodule for bounded live PCM stream buffering,
stream-tap delivery, and stream-core helpers.

- **Owner:** `audio_manager`
- **Visibility:** private implementation headers live under this module; public
  stream contracts remain in `audio_manager/include/`.
- **Lifecycle:** the parent manager remains the sole capture/playback and I2S
  owner.
- **PCM ingress policy:** Xiaozhi PCM starts after a 0.96-second bounded
  prefill (16 packets of at most 960 samples), or sooner at a clean producer
  EOF with queued audio. The 7.68-second PSRAM ring and bounded post-start
  silence/starvation recovery handle later network gaps.
- **Promotion rule:** promote only if the stream core must be reused without the
  parent audio manager lifecycle.
