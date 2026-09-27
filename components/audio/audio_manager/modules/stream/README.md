# Stream module

Private `audio_manager` submodule for bounded live PCM stream buffering,
stream-tap delivery, and stream-core helpers.

- **Owner:** `audio_manager`
- **Visibility:** private implementation headers live under this module; public
  stream contracts remain in `audio_manager/include/`.
- **Lifecycle:** the parent manager remains the sole capture/playback and I2S
  owner.
- **PCM distribution:** the transactional Xiaozhi PTT callback remains one
  bounded consumer. A second, fixed local-monitor consumer has a copied
  4-frame (64 ms / 2,112 B) PSRAM queue. The producer uses zero-wait enqueue;
  a full local queue increments only its local drop counter and cannot apply
  backpressure to PTT or the audio-manager task.
- **Wake-word boundary:** local-monitor registration/enabling never starts RX,
  I2S, a task, ESP-SR, or a network path. It is suppressed during ordinary
  GPIO PTT. `wake_word_manager` can temporarily permit copied frames alongside
  its own accepted WakeNet-to-voice VAD handoff only; this does not create a
  second microphone reader or make generic PTT multi-consumer. It requests the
  continuous capture lifecycle through `audio_manager` arbitration, stops local
  processing during speaker playback, and consumes only copied PCM16 frames
  through the public stream API.
- **PCM ingress policy:** Xiaozhi PCM starts after a 0.96-second bounded
  prefill (16 packets of at most 960 samples), or sooner at a clean producer
  EOF with queued audio. The 7.68-second PSRAM ring and bounded post-start
  silence/starvation recovery handle later network gaps.
- **Promotion rule:** promote only if the stream core must be reused without the
  parent audio manager lifecycle.
