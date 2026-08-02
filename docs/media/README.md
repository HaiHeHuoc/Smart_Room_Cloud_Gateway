# Portfolio Media

This directory is reserved for real target-hardware evidence.

Do not add generated mockups and present them as hardware results. Every image should come from the actual ESP32-S3 prototype or a sanitized tool/database screen captured during a real test.

## Recommended Files

```text
hardware-overview.jpg
screen-provisioning.jpg
screen-wifi.jpg
screen-dashboard.jpg
screen-reset-result.jpg
firebase-latest.png
performance-report.png
```

## Capture Rules

- Remove or blur SSIDs when appropriate.
- Never expose Wi-Fi passwords.
- Blur provisioning QR codes or Proof of Possession values in public material.
- Hide Firebase account email, API credentials, tokens, private database URLs, and personal browser data.
- Record the firmware commit used for each evidence set.
- Prefer landscape images with readable LCD content.
- Avoid reflections and overexposure on the TFT.
- Keep image sizes reasonable for GitHub.

## Video

Host the final video on a suitable public or shareable platform and place the link in the root README.

Recommended sequence:

```text
hardware overview
-> BLE provisioning
-> Wi-Fi status
-> sensor dashboard
-> Firebase update
-> Wi-Fi/cloud recovery
-> factory reset
-> provisioning returns
```

See [`../DEMO.md`](../DEMO.md) for the complete script and acceptance checklist.
