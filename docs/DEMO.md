# Version 1 Hardware Demo

## Published Demo

- Video: [ESP32-S3 Smart Room Cloud Gateway demo](https://youtube.com/shorts/9C5_hecEgXA?feature=share)
- Images: [`docs/media/`](media/README.md)
- Release: `v1.0.0`

The media was captured from the target-hardware project. It is presentation
evidence, not a replacement for raw automated test logs.

## Demonstrated Product Flow

The portfolio sequence covers the main Version 1 story:

```text
hardware overview
-> BLE provisioning
-> Wi-Fi status
-> sensor dashboard
-> Firebase telemetry
-> network/cloud recovery
-> five-second factory reset
-> provisioning returns
```

## Scene Guide

### 1. Hardware Overview

Show:

- ESP32-S3 N16R8 board;
- ST7735 display and integrated microSD slot;
- DHT22 sensor;
- active-low GPIO9 factory-reset button;
- power and wiring.

### 2. BLE Wi-Fi Provisioning

Expected progression:

```text
waiting for phone
-> credentials received
-> connecting
-> waiting for IP
-> saving and read-back verification
-> BLE cleanup
-> connection adoption
-> success
```

Do not expose password entry, a readable Proof of Possession, or a reusable QR
payload in public media.

### 3. Sensor And Firebase

Show temperature and humidity on the LCD and the cloud state returning to
Online. A Firebase Console view is optional; if used, sanitize account identity,
tokens, private project data, and browser information.

### 4. Wi-Fi And Cloud Recovery

From an Online state:

1. Disable the access point or hotspot.
2. Show Wi-Fi disconnect and Cloud Wait/Retry.
3. Restore the network.
4. Show reconnect, IPv4 recovery, Cloud Sync, and Cloud Online.
5. Confirm the newest telemetry value is eventually uploaded.

### 5. Factory Reset

1. Hold GPIO9 for five seconds.
2. Show the reset-result UI.
3. Show the controlled reboot.
4. Confirm BLE provisioning returns without `erase-flash`.

## Published Evidence Matrix

| Evidence | File / URL | Status |
|---|---|---|
| Complete prototype | `docs/media/hardware-overview.jpg` | Published |
| Provisioning screen | `docs/media/screen-provisioning.jpg` | Published |
| Wi-Fi status | `docs/media/screen-wifi.jpg` | Published |
| Sensor dashboard | `docs/media/screen-dashboard.jpg` | Published |
| Reset result | `docs/media/screen-reset-result.jpg` | Published |
| Hardware video | YouTube link above | Published |
| Firebase Console screenshot | `firebase-latest.png` | Optional / not included |
| Performance screenshot | `performance-report.png` | Optional / not included |

## Reproducibility Record

When recording a new demo, capture:

```text
Firmware commit:
Release/tag:
ESP-IDF version:
Board variant:
Power source:
Access point/hotspot:
Provisioning client:
SD card:
Firebase region:
Demo date:
Test duration:
```

Do not record passwords, tokens, private URLs, a readable provisioning secret,
or personal account data.

## Public Media Checklist

- [x] Hardware overview committed.
- [x] Provisioning screen committed.
- [x] Wi-Fi status committed.
- [x] Sensor dashboard committed.
- [x] Reset-result screen committed.
- [x] Demo video uploaded and linked.
- [x] Root README contains the gallery and video link.
- [ ] Optional Firebase screenshot added.
- [ ] Optional performance screenshot added.

The two unchecked optional screenshots are not release blockers.
