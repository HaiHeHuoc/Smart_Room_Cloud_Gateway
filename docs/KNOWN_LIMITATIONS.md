# Known Limitations And Future Work

## Current Known Limitations

### Security And Provisioning

- Firebase development values are configured through menuconfig and compiled into firmware. This is not production secret storage.
- Any credential that previously entered Git history must be rotated; removing it from the current source is not sufficient.
- BLE provisioning currently uses a development Proof of Possession rather than a device-specific manufacturing flow.
- The provisioning QR contains session material and must not be logged or published without sanitization.
- There is no secure element, flash encryption policy, or secure-boot production flow documented yet.

### Cloud

- Telemetry is latest-value only. Samples generated during an outage replace one another rather than forming a historical offline queue.
- The current Firebase endpoint is configured in application composition code and is not yet a portable per-device runtime setting.
- The cloud service has no public stop/deinit/operator-restart API.
- Terminal authentication or deterministic configuration errors remain latched until reboot.
- There is no MQTT, WebSocket dashboard, local web UI, or cloud command channel.

### Storage

- SD card removal is not detected dynamically.
- `sd_card_manager` has no public unmount/deinit API.
- Startup currently treats SD mount failure as fatal for the remaining application flow.
- Automatic formatting is disabled, so an unsupported/unformatted card must be corrected manually.
- The storage service does not yet own generic application data or offline telemetry history.

### UI

- The 160x128 display limits layout density and long text.
- There is no touch input or manual on-device SSID/password entry.
- Provisioning retry is automatic; there is no touch retry button.
- Reset presentation acknowledgment proves an LVGL render cycle, not a physical LCD transfer acknowledgment.
- Real portfolio photos and video are not included until captured from the target hardware.

### Network And Lifecycle

- Full cross-phase provisioning and factory-reset race matrices remain tracked in the roadmap.
- Provisioning retry count and timing are fixed application configuration values.
- Runtime Wi-Fi failure does not automatically start provisioning, by design.
- The application currently has one long-lived cloud task and no runtime cloud-service restart command.

### Hardware

- Pin assignments are board-specific and must be reviewed before changing the ESP32-S3 module or carrier.
- The DHT22 is slow and less accurate than newer digital sensors.
- The current SD clock is intentionally conservative.
- No card-detect input is used.
- Audio hardware and GPIO allocation are not yet accepted; voice work begins only in the optional post-MVP roadmap.

### Resource Measurement

- Performance reports are workload snapshots, not guaranteed fixed values.
- Internal and DMA-capable heap totals overlap and must not be added.
- Fragmentation percentages are diagnostic estimates.
- Low-water stack and heap values need renewed validation whenever tasks, networking, GUI, audio, or dependency versions change.

## Recommended Future Improvements

### Near-Term Portfolio Improvements

1. Add real hardware photos and sanitized screenshots.
2. Upload a two-to-four-minute demo video.
3. Select and add an explicit repository license.
4. Add a reproducible test-results document with firmware commit and hardware matrix.
5. Replace any remaining project-specific Firebase endpoint with configurable application settings.
6. Review and rewrite Git history if the repository will be published broadly, after rotating exposed credentials.

### Product Hardening

1. Device-specific provisioning secret stored in protected manufacturing data.
2. Flash encryption and secure boot threat-model review.
3. Protected device identity and cloud credentials.
4. OTA with project-owned validation, rollback, and version policy.
5. Runtime service stop/deinit/restart APIs.
6. Card-detect, unmount, and storage-error recovery.
7. Watchdog and health-policy integration beyond diagnostics.
8. Automated hardware-in-the-loop regression where practical.

### Data And Cloud

1. SNTP time synchronization and real timestamps.
2. SD-backed bounded offline history.
3. Batched historical upload after connectivity returns.
4. Configurable telemetry interval and endpoint.
5. MQTT only when its operational value exceeds the added lifecycle and security cost.

### Sensor And UI

1. Replace DHT22 with a more accurate sensor after preserving the manager API.
2. Add historical charting only after measuring LVGL memory impact.
3. Add user-safe configuration and diagnostics screens.
4. Add brightness control through a project-owned API.
5. Improve accessibility and long-text handling on the small display.

### Optional Voice Extension

The approved optional roadmap places voice after the existing MVP:

```text
Sprint 10: audio hardware validation
Sprint 11: production audio manager
Sprint 12: Xiaozhi build and transport validation
Sprint 13: voice assistant adapter
Sprint 14: push-to-talk MVP
Sprint 15: GUI voice integration
Sprint 16: MCP read-only tools
Sprint 17: controlled MCP actions
Sprint 18: wake word and advanced voice UX
```

This extension must not replace existing Wi-Fi, provisioning, storage, cloud, GUI, or reset ownership.

## Deliberately Deferred Features

- Custom mobile application
- Firestore direct integration
- Always-on BLE data streaming
- Local web dashboard
- OTA before a complete security and rollback design
- Wake word before push-to-talk and audio resource acceptance
- AI-controlled destructive actions such as reset, credential erase, reboot, or OTA
