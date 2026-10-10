# Known Limitations And Deferred Work

This document separates implemented design restrictions, evidence that is not
yet collected, and deliberately deferred product work. A missing target test is
not by itself a confirmed functional defect.

## Security

- Firebase development credentials are compiled into local development firmware;
  the project does not provide hardware-backed secret storage.
- Secure boot, flash encryption, NVS encryption, signed OTA, fleet identity,
  revocation, and credential rotation are not implemented as a verified product
  deployment policy.
- Removing data from the current tree does not remove data reachable through
  public Git history. Credential rotation and history assessment remain owner
  release responsibilities.

## Hardware

- GPIO assignments are specific to the documented ESP32-S3 board/carrier.
- DHT22 accuracy and update rate are limited compared with newer sensors.
- The SD bus uses a conservative clock and has no card-detect input.
- GPIO38 PTT requires its documented external pull-down for a long/noisy wire.

## Audio

- WAV playback accepts only PCM integer, mono, 16 kHz, 16-bit RIFF/WAVE input.
- The current FAT VFS/C stdio path supports files smaller than 2 GiB; larger
  files require a deliberate 64-bit-safe reader migration.
- WAV playback has bounded PSRAM prefetch storage, not whole-file loading.
- A synchronous SD/VFS read or I2S write cannot be preempted mid-call; shutdown
  is cooperative and reports its finite timeout rather than force-deleting an
  owner task.
- Target confirmation of sound quality, SD latency under combined load, and
  long-duration resource stability remains separate from host/source checks.

## Xiaozhi And Voice

- The product selects managed Xiaozhi WebSocket transport. MQTT+UDP is not a
  supported fallback in this release.
- First-boot activation is backend-authoritative. It is not persisted in NVS;
  activation-required state blocks PTT and session startup until the backend
  response changes.
- A session/request admission does not by itself prove an audible speaker result
  or a complete conversation turn.
- Some real network, service-loss, browser, endurance, and audio acceptance
  scenarios remain `NOT VERIFIED` until owner-provided target evidence exists.

## MCP

- MCP providers use bounded copied state and owner APIs; they do not expose
  driver handles, arbitrary file paths, credentials, or raw transport data.
- `cloud.push_latest` reports scheduling/admission only. Upload completion must
  be observed through the cloud owner status.
- `audio.play_track` and `audio.play_recorded` can report accepted scheduling
  before playback is confirmed by later manager state or target observation.

## Local Web

- The Local Web server starts only when enabled and after application online
  handoff; it does not configure or reconnect Wi-Fi.
- It is not a browser microphone, WebRTC, PCM/Opus, or direct Xiaozhi transport
  client.
- Storage and catalog views are bounded snapshots. Media operations remain
  subject to SD availability, lease ownership, and audio arbitration.

## Cloud

- The gateway uploads latest-value telemetry; it is not a durable offline
  history or fleet synchronization system.
- Firebase account configuration, database rules, API restrictions, and quotas
  are deployment responsibilities. Cloud status does not prove Internet health
  or that a newer sample has completed upload.

## Storage

- SD failure/recovery is asynchronous. Consumers must use the SD lease contract
  rather than assume a permanently mounted VFS.
- Automatic formatting is disabled to protect card data.
- Persistent logs and files can contain sensitive operational information and
  require sanitization before publication.

## UI

- The ST7735 160×128 logical layout limits text density and complex views.
- There is no touch UI, local SSID/password editor, or browser-to-device audio
  capture path.
- LVGL is task-owned; producer callbacks may only post/copy models.

## Memory And Performance

- Performance reports are workload snapshots, not fixed guarantees.
- Internal and DMA-capable heap capabilities overlap and must not be summed.
- Fragmentation values are diagnostic indicators; stack high-water values must
  be revalidated after lifecycle, audio, network, or UI changes.

## Lifecycle And Recovery

- Long-lived services use component-owned cooperative cleanup. A partial or
  failed owner cleanup can intentionally fail closed rather than free resources
  beneath a live task.
- Wi-Fi, cloud, storage, audio, UI, and managed Xiaozhi recovery have separate
  ownership. One component must not restart another component's private task or
  handle.

## Deferred Features

- Wake word, VAD, continuous capture, and advanced voice UX are suspended and
  are not Version 2.0.0 features.
- Secure OTA, mobile application, fleet provisioning, and configurable cloud
  history are future design work.
- Additional owner media and target-HIL matrices are pending evidence, not
  implemented release features.
