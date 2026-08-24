# ADR: Xiaozhi WebSocket-Only Transport

**Status:** Accepted for Phase 12 validation and the current project roadmap.

## Context

The reviewed `esp_xiaozhi` integration exposed both MQTT and WebSocket service
configuration. The project first exercised the official MQTT path in the
Gateway and then reproduced the same pre-`CONNECTED` sequence in a standalone,
official-flow ESP-IDF test:

```text
Certificate validated
transport_read(): EOF
errno=119
mqtt_message_receive()=-2
```

The evidence shows that the MQTT path was unusable in the tested environment
after TLS validation and before a usable chat connection. It does **not** prove
that a remote MQTT broker is defective: broker-side logs and server ownership
were not available to this project.

WebSocket is the selected path because it is available through the pinned
public `esp_xiaozhi` API and has an explicit Phase 12 validation path. This is
an architectural scope decision, not a claim that every WebSocket lifecycle or
audio interaction has passed target-hardware acceptance.

## Decision

The Gateway selects **WebSocket only** for Xiaozhi.

- Project transport selection exposes no MQTT enum, capability, endpoint, or
  fallback path.
- `xiaozhi_foundation` ignores upstream MQTT capability and configures chat
  with `has_mqtt_config = false` and `has_websocket_config = true`.
- Upstream-managed MQTT dependency and private NVS state may remain as an
  implementation detail. The project neither reads, logs, mirrors, nor starts
  that transport.
- Later WebSocket failure does not authorize an MQTT fallback. A different
  transport would require a new reviewed ADR and a separately scoped phase.

## Consequences

- Phase 12 evidence and resource diagnostics are intentionally WebSocket-only.
- P2-E verifies the WebSocket audio-channel lifecycle only when its complete
  target serial trace is captured.
- P2-F remains gated by a lawful local raw-Opus fixture and requires both
  serial conversation/audio evidence and audible target proof.
- Phase 12.6 owns staged repeated WebSocket lifecycle/cleanup/resource
  evidence and default-off, project-owned controlled abort/recovery checks at
  safe public-API boundaries. They do not mutate private transport state or
  authorize an MQTT fallback.
- Real Wi-Fi/AP, Internet/DNS/TLS/service loss, server goodbye, remote timeout,
  malformed remote response, and allocation-pressure evidence remain separate
  source-audited or target-hardware work. They are not replaced by a fake
  project transport fault and do not authorize a transport fallback.
- This decision does not add production voice-assistant behavior, microphone
  capture, speaker playback, MQTT+UDP, or a production GUI feature.

## Evidence Boundary

The MQTT reproduction is sufficient to keep MQTT outside the project-selected
architecture. It is not remote-service diagnosis. The next hardware target is
P2-C/P2-E/P2-F WebSocket evidence on the ESP32-S3, collected according to
[Xiaozhi hardware acceptance data](XIAOZHI_HARDWARE_ACCEPTANCE.md).
