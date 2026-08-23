# Known Limitations And Future Work

## Version 1 Product Boundaries

### Security And Provisioning

- Firebase development values are entered through menuconfig and compiled into
  firmware. This is not production secret storage.
- Any password or token that previously entered Git history must be rotated;
  removing it from the current tree does not invalidate old commits or clones.
- BLE provisioning uses a development Proof of Possession rather than a
  manufacturing-time unique secret.
- The provisioning QR contains session material and must be sanitized before
  public publication.
- Version 1 does not define a secure-element, NVS-encryption, flash-encryption,
  secure-boot, or signed-OTA production policy.
- Firebase App Check is not implemented by the ESP-IDF client.

### Cloud And Data

- Telemetry is latest-value only. Samples produced during an outage overwrite
  one another rather than creating an offline history.
- The Firebase database endpoint is configured in application composition code
  rather than a portable deployment profile.
- The cloud service has no public stop/deinit/operator-restart API.
- Terminal authentication or deterministic configuration errors remain latched
  until reboot.
- There is no MQTT, local web dashboard, cloud command channel, or historical
  chart service.

### Storage

- The board has no card-detect or SD power-enable GPIO. Idle physical card
  removal and firmware power-cycling cannot be guaranteed; recovery uses a
  best-effort idle status probe plus managed VFS I/O failures.
- `sd_card_manager` has no public stop/deinit API; its recovery task is
  designed for the firmware lifetime.
- Startup no longer treats SD mount failure as fatal. The manager retries in
  the background, but every SD VFS consumer must use its lease contract or a
  runtime unmount would be unsafe.
- Automatic formatting is disabled to protect card data.
- Storage does not own offline telemetry history.

### UI

- The 160x128 logical layout limits text density and complex visualization.
- There is no touch input or on-device SSID/password editor.
- Provisioning retry is automatic; there is no touch retry control.
- Reset-result acknowledgment confirms an LVGL render cycle, not physical LCD
  transfer completion.

### Network And Lifecycle

- Provisioning retry count and timing are fixed application values.
- Runtime Wi-Fi failure intentionally reconnects instead of automatically
  reopening provisioning.
- Long-lived services are one-shot and have no coordinated runtime shutdown.
- The current architecture is validated for one gateway and one Firebase
  identity, not fleet provisioning or fleet credential rotation.

### Hardware

- GPIO assignments are board-specific and must be reviewed when changing the
  ESP32-S3 board or carrier.
- DHT22 accuracy and update speed are limited compared with newer sensors.
- The SD clock is intentionally conservative.
- No card-detect input is used.
- Audio hardware and GPIO allocation belong to Version 2 and are not part of
  the Version 1 release.

### Resource Measurements

- Performance reports are workload snapshots, not fixed guarantees.
- Internal and DMA-capable heap capabilities overlap and must not be added.
- Fragmentation percentages are diagnostic estimates.
- Heap and stack low-water values must be revalidated after task, network, GUI,
  audio, or dependency changes.

## Public-Release Requirements

The source tree can be prepared for publication, but repository visibility
must not be changed until the owner completes the actions in `SECURITY.md`,
including:

1. rotate every credential previously committed;
2. confirm old credentials are revoked;
3. inspect or rewrite Git history for still-sensitive values;
4. publish restrictive Firebase Realtime Database rules;
5. review API-key restrictions and quotas;
6. sanitize all media and logs;
7. avoid publishing credential-bearing firmware binaries.

## Recommended Future Improvements

### Release Engineering

1. Add an explicit repository license.
2. Add CI for formatting, build configuration, and host-side checks.
3. Add automated secret scanning.
4. Publish release tags and a reproducible build record.
5. Add sanitized Firebase and performance screenshots when useful.

### Product Hardening

1. Unique per-device identity and credential rotation.
2. Protected manufacturing and provisioning data.
3. NVS encryption, flash encryption, and secure boot.
4. Signed OTA with validation, rollback, and version policy.
5. Runtime service stop/deinit/restart APIs.
6. Hardware card-detect and SD power control for deterministic hot-plug
   recovery.
7. Watchdog and health policy beyond passive diagnostics.
8. Hardware-in-the-loop regression where practical.
9. App Check or a project-owned authenticated backend for abuse control.

### Data And Cloud

1. SNTP synchronization and real timestamps.
2. SD-backed bounded offline history.
3. Batched historical upload after connectivity returns.
4. Configurable telemetry interval, device ID, and endpoint.
5. Fleet-safe per-device database paths and revocation.

### Sensor And UI

1. Replace DHT22 behind the existing manager API.
2. Add historical charts only after measuring LVGL memory impact.
3. Add user-safe configuration and diagnostics screens.
4. Add brightness control through a project-owned API.
5. Improve accessibility and long-text handling.

## Version 2 Voice Extension

The approved post-Version 1 order is:

```text
Sprint 10: audio hardware validation
Sprint 11: production audio manager
Sprint 12: Xiaozhi build and WebSocket transport validation
Sprint 13: voice assistant adapter
Sprint 14: push-to-talk MVP
Sprint 15: GUI voice integration
Sprint 16: MCP read-only tools
Sprint 17: controlled MCP actions
Sprint 18: wake word and advanced voice UX
```

Version 2 must not replace existing Wi-Fi, provisioning, storage, cloud, GUI,
or reset ownership.

### Xiaozhi Transport Boundary

- Xiaozhi MQTT+UDP is intentionally **not selected** for the current roadmap.
- The same pre-CONNECTED MQTT failure (`Certificate validated` followed by
  `transport_read(): EOF`, `errno=119`, and `mqtt_message_receive()=-2`) was
  reproduced in both the Gateway integration and a standalone official-flow
  `esp_xiaozhi` test.
- This evidence is sufficient to classify the MQTT path as unusable in the
  validated environment; it does not prove the remote broker itself defective
  without broker/server logs.
- Project-side MQTT availability and MQTT transport-selection attributes are
  removed. The project selects WebSocket only and does not provide MQTT
  fallback.
- `esp_xiaozhi` may still contain its own MQTT dependency/private NVS state as
  upstream implementation detail. The Gateway does not read, expose, or start
  that transport.
- Remaining voice validation is WebSocket-only: control/text, PCM/audio,
  reconnect, cleanup, stress, and resource measurements.

## Deliberately Deferred

- Custom mobile application
- Firestore direct integration
- Always-on BLE data streaming
- Local web dashboard
- OTA before a complete security and rollback design
- Xiaozhi MQTT+UDP transport
- Wake word before push-to-talk and audio resource acceptance
- AI-controlled destructive actions such as reset, credential erase, reboot, or OTA
