# Version 1 Portfolio Media Placeholders

Version 1 is complete. The entries below are optional presentation updates and do not block the `v1.0.0` release.

## Hardware And LCD Images

HaiHeHuoc888: update here + add `docs/media/hardware-overview.jpg` showing the complete ESP32-S3 prototype, LCD, DHT22, SD module, button, and wiring clearly.

HaiHeHuoc888: update here + add `docs/media/screen-provisioning.jpg` showing the BLE provisioning screen; blur or cover the QR payload and Proof of Possession before public publication.

HaiHeHuoc888: update here + add `docs/media/screen-wifi.jpg` showing the connected Wi-Fi status screen; blur the SSID when needed.

HaiHeHuoc888: update here + add `docs/media/screen-dashboard.jpg` showing temperature, humidity, Wi-Fi summary, and cloud state on the sensor dashboard.

HaiHeHuoc888: update here + add `docs/media/screen-reset-result.jpg` showing the factory-reset result screen before reboot-to-provisioning.

## Tool And Cloud Screenshots

HaiHeHuoc888: update here + add `docs/media/firebase-latest.png` showing the latest Firebase telemetry object with account data, tokens, credentials, private URLs, and personal browser information removed.

HaiHeHuoc888: update here + add `docs/media/performance-report.png` showing a representative performance-monitor report with CPU, Internal RAM, PSRAM, DMA-capable RAM, task count, stack high-water marks, and stack locations.

## Video

HaiHeHuoc888: update here + add the public or shareable Version 1 demo video URL below.

```text
Video URL: <update here>
```

Recommended sequence:

```text
hardware overview
-> boot
-> BLE provisioning QR
-> Wi-Fi connected
-> sensor dashboard
-> Firebase telemetry update
-> hotspot off/on recovery
-> long-press factory reset
-> provisioning screen returns
```

## Root README Gallery

HaiHeHuoc888: update here + add a `Version 1 Demo` or `Gallery` section to the root `README.md` and embed the selected images plus the final video link.

Suggested Markdown after the files exist:

```markdown
## Version 1 Demo

![Hardware overview](docs/media/hardware-overview.jpg)
![Provisioning screen](docs/media/screen-provisioning.jpg)
![Sensor dashboard](docs/media/screen-dashboard.jpg)

Demo video: <public-or-shareable-url>
```

## Sanitization Checklist

- [ ] No Wi-Fi password is visible.
- [ ] SSIDs are blurred when privacy requires it.
- [ ] Provisioning QR payload and Proof of Possession are hidden.
- [ ] Firebase passwords, tokens, API credentials, and private URLs are hidden.
- [ ] Personal email, browser profile, tabs, notifications, and account information are hidden.
- [ ] The firmware commit or release tag used for the media is recorded.
- [ ] Images come from the real target hardware or a real sanitized test session.
