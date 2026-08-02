# Portfolio Demo Guide

## Goal

Produce a short, repeatable hardware demonstration that proves the project works end to end without exposing credentials, provisioning secrets, tokens, or private account information.

## Recommended Duration

Target: 2-4 minutes.

A shorter edited video is more useful for a portfolio than an unstructured serial-monitor recording.

## Required Preparation

- Flash the portfolio revision.
- Insert the FAT-formatted SD card.
- Use a dedicated Firebase development account.
- Hide email addresses, tokens, API keys, QR contents, SSID/password input, and private database URLs.
- Clear or blur unrelated serial output and desktop notifications.
- Record the Git commit used for the demo.
- Confirm the LCD is readable under the selected lighting.

## Primary Demo Script

### 1. Hardware Overview — 10-15 seconds

Show:

- ESP32-S3 board
- ST7735 display
- DHT22
- microSD module/card
- factory-reset button

Brief narration:

> This is an ESP32-S3 Smart Room Cloud Gateway built with ESP-IDF and FreeRTOS. It uses BLE for Wi-Fi provisioning, LVGL for the local dashboard, a DHT22 sensor, Firebase telemetry, NVS storage, and automatic recovery.

### 2. BLE Provisioning — 30-45 seconds

1. Boot with no valid Wi-Fi configuration.
2. Show the provisioning screen and QR code.
3. Scan with the provisioning client.
4. Do not record the password entry.
5. Submit the credentials.
6. Show status progression:

```text
Waiting for phone
-> credentials received
-> connecting
-> waiting for IP
-> saving
-> cleaning up
-> success
```

Evidence to capture:

- QR displayed correctly.
- Session count is visible.
- Device receives IPv4.
- UI routes to Wi-Fi status and then dashboard.

### 3. Sensor And Firebase — 20-30 seconds

1. Show temperature and humidity updates on the LCD.
2. Show the Firebase latest-value record changing.
3. Blur private project/account details.

Brief narration:

> The sensor task publishes copied data to the UI and a latest-value cloud queue. The cloud task authenticates and uploads through HTTPS without blocking the sensor or UI tasks.

### 4. Wi-Fi And Cloud Recovery — 30-45 seconds

1. Begin from Cloud Online.
2. Turn the hotspot/router off.
3. Show Wi-Fi disconnect and Cloud Wait/Retry.
4. Turn the hotspot/router on.
5. Show Wi-Fi reconnect, IPv4 recovery, Cloud Sync, and Cloud Online.

Evidence to capture:

- No reboot or provisioning restart.
- GUI remains responsive.
- Firebase receives the newest sensor value after recovery.

### 5. Factory Reset — 25-40 seconds

1. Hold the reset button for five seconds.
2. Show reset-result UI.
3. Show reboot.
4. Show the provisioning screen again.

Brief narration:

> The button task only publishes an event. A reset coordinator quiesces network activity, erases verified Wi-Fi persistence, presents the result through the UI task, and restarts into provisioning.

### 6. Runtime Diagnostics — 15-20 seconds

Show one performance report containing:

- CPU use
- Internal RAM
- PSRAM
- DMA-capable RAM
- task count
- selected task stack locations

Highlight:

```text
app_gui_ui      PSRAM
cloud_manager   PSRAM
sensor_manager  PSRAM
button_manager  PSRAM
perf_monitor    INTERNAL
```

## Screenshot Checklist

Capture at least these still images:

| File | Content |
|---|---|
| `hardware-overview.jpg` | Full wired prototype |
| `screen-provisioning.jpg` | Provisioning screen with QR blurred if needed |
| `screen-wifi.jpg` | Wi-Fi status screen without private SSID if publishing publicly |
| `screen-dashboard.jpg` | Sensor and cloud dashboard |
| `screen-reset-result.jpg` | Factory-reset result screen |
| `firebase-latest.png` | Sanitized Firebase latest-value record |
| `performance-report.png` | Sanitized runtime diagnostics |

Store files in `docs/media/`.

## README Media Block

After adding real files, insert a compact gallery into the root README:

```markdown
## Hardware Demo

| Prototype | Dashboard |
|---|---|
| ![Prototype](docs/media/hardware-overview.jpg) | ![Dashboard](docs/media/screen-dashboard.jpg) |

[Watch the demo video](<public-video-url>)
```

Do not add broken image links before the files exist.

## Evidence Record

For a reproducible portfolio demo, record:

```text
Firmware commit:
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

Do not include passwords, API credentials, token contents, provisioning Proof of Possession, or unredacted private identifiers.

## Acceptance Checklist

- [ ] Real hardware overview photo committed.
- [ ] Provisioning screenshot/photo committed.
- [ ] Sensor dashboard screenshot/photo committed.
- [ ] Sanitized Firebase evidence committed.
- [ ] Sanitized performance evidence committed.
- [ ] Demo video uploaded to a public or shareable location.
- [ ] Root README updated with real media links.
- [ ] No secrets or personal information visible.
- [ ] Video demonstrates provisioning, sensor/cloud, reconnect, and reset.
