# Version 1 Media

This directory contains target-hardware evidence for the ESP32-S3 Smart Room
Cloud Gateway `v1.0.0` release.

## Demo Video

[Watch the Version 1 hardware demo](https://youtube.com/shorts/9C5_hecEgXA?feature=share)

The video demonstrates the real prototype and representative Version 1 flows.
The root README uses the dashboard image as the clickable video thumbnail.

## Published Images

| File | Evidence |
|---|---|
| `hardware-overview.jpg` | Complete wired ESP32-S3 prototype |
| `screen-provisioning.jpg` | BLE Wi-Fi provisioning screen |
| `screen-wifi.jpg` | Wi-Fi status screen |
| `screen-dashboard.jpg` | Sensor and cloud dashboard |
| `screen-reset-result.jpg` | Factory-reset result screen |

### Gallery

| Prototype | Provisioning |
|---|---|
| ![Prototype](hardware-overview.jpg) | ![Provisioning](screen-provisioning.jpg) |

| Wi-Fi | Dashboard | Reset result |
|---|---|---|
| ![Wi-Fi](screen-wifi.jpg) | ![Dashboard](screen-dashboard.jpg) | ![Reset result](screen-reset-result.jpg) |

## Public Sanitization Rules

Before replacing or adding media:

- remove or blur SSIDs when privacy requires it;
- never expose Wi-Fi passwords;
- hide provisioning QR payloads and Proof of Possession values;
- hide Firebase email addresses, passwords, ID tokens, refresh tokens, API
  credentials, private database data, and personal browser information;
- remove unrelated desktop notifications and account avatars;
- do not publish firmware binaries or serial logs containing real credentials;
- record the firmware commit used for the evidence when reproducibility matters;
- use real target-hardware images, not generated mockups presented as evidence.

## Optional Future Evidence

The current release does not require additional media. Useful future additions
include:

```text
firebase-latest.png
performance-report.png
```

Any Firebase screenshot must be sanitized and must not reveal account identity,
tokens, database rules containing a real UID, or private project data.

See [`../DEMO.md`](../DEMO.md) for the demonstration flow.
