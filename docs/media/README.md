# Product Media

## Version 1 Evidence

This directory retains real target-hardware evidence for the Version 1 release.

| File | Evidence |
|---|---|
| `hardware-overview.jpg` | Complete wired ESP32-S3 prototype |
| `screen-provisioning.jpg` | BLE Wi-Fi provisioning screen |
| `screen-wifi.jpg` | Wi-Fi status screen |
| `screen-dashboard.jpg` | Sensor and cloud dashboard |
| `screen-reset-result.jpg` | Factory-reset result screen |

[Watch the Version 1 hardware demo](https://youtube.com/shorts/9C5_hecEgXA?feature=share).

## Version 2 Evidence

Version 2 image, video, and audio paths are reserved in
[V2 Media Assets](V2_MEDIA_ASSETS.md). The `v2/` directory contains no
placeholder evidence: every listed missing asset is intentionally
`PENDING OWNER UPLOAD` until the owner supplies a sanitized real capture.

## Public Sanitization Rules

- Remove or blur SSIDs, QR/PoP data, credentials, tokens, private database
  values, browser profiles, account information, and unrelated notifications.
- Do not publish firmware binaries, local `sdkconfig`, raw secret-bearing logs,
  or private conversations.
- Label a capture with the release commit when reproducibility matters.
- Do not present generated imagery, mock screenshots, or simulated logs as
  target-hardware evidence.

See [the demo guide](../DEMO.md) for the reproducible capture sequence.
