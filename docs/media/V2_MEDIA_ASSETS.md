# Version 2 Media Assets

This manifest records the stable paths for Version 2 evidence. The listed
assets are owner-supplied captures; their presence documents available media,
not a new claim of hardware acceptance beyond the capture itself.

## Capture Rules

- Use real target hardware and the stated release commit.
- Remove or blur SSIDs, QR/PoP payloads, account identifiers, tokens, private
  database data, browser profiles, and unrelated notifications.
- Do not upload firmware images, credentials, or unsanitized serial logs.
- Do not describe a request acknowledgement as cloud completion or audible
  output unless the capture demonstrates the later result.

## Canonical Asset Set

| Asset ID | Media type | Priority | Exact repository path or external link | Purpose and capture requirement | Referenced documentation | Status |
|---|---|---|---|---|---|---|
| V2-HW-01 | Image | REQUIRED | `docs/media/v2/v2-hardware-overview.jpg` | Complete powered prototype; show display, SD, audio hardware, and controls without secrets. | README, DEMO | AVAILABLE |
| V2-LCD-01 | Image | REQUIRED | `docs/media/v2/v2-lcd-dashboard.jpg` | LCD dashboard with normal sensor/network state. | README, DEMO | AVAILABLE |
| V2-ACT-01 | Image | REQUIRED | `docs/media/v2/v2-xiaozhi-activation.jpg` | Activation-required screen with any sensitive code redacted when publication requires it. | SETUP, DEMO | AVAILABLE |
| V2-VOICE-01 | Video | REQUIRED | [YouTube Short](https://youtube.com/shorts/rdfI0ZxzbNA?feature=share) | GPIO38 PTT turn through visible state transition and audible/semantic result. | DEMO | AVAILABLE EXTERNALLY |
| V2-WEB-01 | Image | REQUIRED | `docs/media/v2/v2-local-web-overview.jpg` | Local Web landing/dashboard from the device network; sanitize browser data. | README, DEMO | AVAILABLE |
| V2-DEMO-01 | Video | REQUIRED | [YouTube video](https://youtu.be/r4Q5xO2WkmA) | Short end-to-end V2 walkthrough using the documented demo sequence. | README, DEMO, release record | AVAILABLE EXTERNALLY |
| V2-AUDIO-01 | Image | RECOMMENDED | `docs/media/v2/v2-audio-hardware.jpg` | INMP441/MAX98357A wiring and physical audio path. | DEMO | PENDING OWNER UPLOAD |
| V2-MCP-01 | Image | RECOMMENDED | `docs/media/v2/v2-mcp-sensor-query.jpg` | Xiaozhi interaction showing a sensor query and bounded result. | DEMO | PENDING OWNER UPLOAD |
| V2-MCP-02 | Image | RECOMMENDED | `docs/media/v2/v2-mcp-light-control.jpg` | Xiaozhi MCP light command with visible NeoPixel result. | DEMO | PENDING OWNER UPLOAD |
| V2-MCP-03 | Image | RECOMMENDED | `docs/media/v2/v2-mcp-audio-playback.jpg` | Accepted audio-control/playback state with outcome labeled accurately. | DEMO | PENDING OWNER UPLOAD |
| V2-WEB-02 | Image | RECOMMENDED | `docs/media/v2/v2-local-web-storage.jpg` | Storage listing/upload/download workflow with safe filenames only. | DEMO | AVAILABLE |
| V2-WEB-03 | Image | RECOMMENDED | `docs/media/v2/v2-local-web-playback.jpg` | Playback and volume controls with a valid track. | DEMO | AVAILABLE |
| V2-WEB-04 | Image | RECOMMENDED | `docs/media/v2/v2-local-web-lights.jpg` | Light control UI with observed device state. | DEMO | AVAILABLE |
| V2-WEB-05 | Image | RECOMMENDED | `docs/media/v2/v2-local-web-scenes.jpg` | Scene list/application result; identify partial/busy outcomes honestly. | DEMO | NOT PROVIDED |
| V2-WEB-06 | Image | RECOMMENDED | `docs/media/v2/v2-local-web-logs.jpg` | Sanitized Local Web logs page. | DEMO | AVAILABLE |
| V2-WEB-07 | Image | RECOMMENDED | `docs/media/v2/v2-local-web-diagnostic.jpg` | Sanitized Local Web diagnostics page. | DEMO | AVAILABLE |
| V2-WEB-08 | Image | RECOMMENDED | `docs/media/v2/v2-local-web-Xiaozhi.jpg` | Local Web Xiaozhi control/status page. | DEMO | AVAILABLE |
| V2-CONFIG-01 | Image | REFERENCE | `docs/media/v2/v2_menuconfig.jpg` | Sanitized ESP-IDF Menuconfig capture. | README | AVAILABLE |
| V2-DESIGN-01 | Image | REFERENCE | `docs/media/v2/v2_Schematic.png` | Rendered hardware schematic. | README | AVAILABLE |
| V2-DESIGN-02 | Source | REFERENCE | `docs/media/v2/v2_Schematic.mmd` | Editable Mermaid schematic source. | README | AVAILABLE |
| V2-CLOUD-01 | Image | OPTIONAL | `docs/media/v2/v2-cloud-sync.jpg` | Sanitized cloud-sync/status evidence. | DEMO | PENDING OWNER UPLOAD |
| V2-AUDIO-02 | Audio | OPTIONAL | `docs/media/v2/v2-voice-sample.wav` | Sanitized optional voice/output sample; do not include private conversation. | DEMO | PENDING OWNER UPLOAD |
| V2-DIAG-01 | Image | OPTIONAL | `docs/media/v2/v2-system-diagnostics.jpg` | Runtime status/resource snapshot with no credentials or identifiers. | DEMO | PENDING OWNER UPLOAD |

## External Video Evidence

- [V2 PTT demonstration](https://youtube.com/shorts/rdfI0ZxzbNA?feature=share)
- [V2 full demonstration](https://youtu.be/r4Q5xO2WkmA)

The videos are deliberately hosted externally and are not stored in this
repository. The optional voice sample remains `PENDING OWNER UPLOAD`.
