# Setup And Build Guide

## Supported Baseline

| Item | Version / configuration |
|---|---|
| MCU | ESP32-S3 N16R8 |
| Framework | ESP-IDF 6.0.1 |
| Flash | 16 MB |
| PSRAM | 8 MiB Octal, 80 MHz |
| Display | ST7735 128x160 SPI TFT |
| Sensor | DHT22 |
| Storage | FAT-formatted microSD |
| Input | Active-low GPIO9 reset button |

## 1. Install And Activate ESP-IDF

Install ESP-IDF 6.0.1 using Espressif's installer or command-line workflow.
Activate its environment and confirm:

```bash
idf.py --version
```

## 2. Clone And Select The Target

```bash
git clone https://github.com/HaiHeHuoc/Smart_Room_Cloud_Gateway.git
cd Smart_Room_Cloud_Gateway
idf.py set-target esp32s3
```

Managed dependencies are resolved from `main/idf_component.yml` during
configuration and build.

## 3. Wire The Hardware

### ST7735 LCD

| Signal | ESP32-S3 GPIO |
|---|---:|
| MOSI / SDA | 11 |
| SCLK / SCL | 12 |
| CS | 10 |
| DC / A0 | 13 |
| RST / RES | 14 |
| BL / LED | 15 |
| MISO | Not used by LCD |

### Integrated microSD Interface

| Signal | ESP32-S3 GPIO |
|---|---:|
| MOSI | 16 |
| MISO | 17 |
| SCLK | 18 |
| CS | 8 |

### Sensor And Button

| Function | ESP32-S3 GPIO | Notes |
|---|---:|---|
| DHT22 data | 4 | Use the required pull-up for the selected module |
| Factory-reset button | 9 | Active low, internal pull-up, five-second hold |

Use 3.3 V logic and a common ground. Confirm the source-of-truth definitions in
`components/system/common/include/board_config.h` before applying power.

## 4. Prepare The SD Card

- Format the card with a supported FAT filesystem.
- Insert it before boot.
- Automatic formatting is disabled.
- The current SDSPI clock is intentionally conservative at 2 MHz.

The card mounts at `/sdcard`; LVGL exposes it through the `S:` drive.

## 5. Configure Firebase Securely

Follow the complete guide:

- [`components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md`](../components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)

The minimum steps are:

1. Enable Firebase Email/Password Authentication.
2. Create a dedicated device user and record its UID.
3. Create Realtime Database.
4. Publish restrictive `auth.uid` Security Rules.
5. Review Firebase API-key restrictions and quotas.
6. Run `idf.py menuconfig` and set local development values under:

```text
Smart Room Cloud Gateway
└── Firebase development configuration
```

The generated `sdkconfig` is ignored by Git, but its values are compiled into
the firmware image. Never publish a firmware binary built with real credentials.

## 6. Build

```bash
idf.py build
```

For a clean rebuild:

```bash
idf.py fullclean
idf.py build
```

## 7. Flash And Monitor

```bash
idf.py -p <PORT> flash monitor
```

Typical port names:

```text
Windows: COM5
Linux:   /dev/ttyACM0
macOS:   /dev/cu.usbmodemXXXX
```

Exit the monitor with `Ctrl+]`.

## 8. First Boot

### Stored Wi-Fi Configuration Is Valid

```text
boot
-> display, LVGL, SD, and configuration initialization
-> Wi-Fi connect and IPv4
-> Wi-Fi status screen
-> sensor dashboard
-> Firebase authentication and telemetry
```

### No Valid Wi-Fi Configuration

```text
boot
-> BLE provisioning screen and QR
-> phone sends credentials
-> Wi-Fi and IPv4 verification
-> NVS save and read-back
-> BLE cleanup and Station adoption
-> Wi-Fi status and sensor dashboard
```

Use an Espressif-compatible provisioning client that supports BLE transport and
Security 1. Do not publish a readable provisioning QR or Proof of Possession.

## 9. Functional Verification

### Display

- Correct orientation and RGB order.
- No clipping on the 160x128 logical layout.
- Provisioning QR is scannable.
- Sensor, Wi-Fi, and cloud states update without freezing.

### Sensor

- Temperature and humidity update approximately every two seconds.
- Invalid readings enter stale/error handling without crashing the UI.

### Firebase

- Authentication succeeds with the dedicated device account.
- The configured UID guard accepts the expected identity.
- Latest telemetry changes in Realtime Database.
- Anonymous database access is rejected.
- Wi-Fi loss enters Wait/Retry and recovery returns to Online.

### Factory Reset

1. Hold GPIO9 for five seconds.
2. Verify the reset-result screen.
3. Verify reboot.
4. Verify the device returns to provisioning.
5. Reprovision without running `erase-flash`.

## 10. Runtime Diagnostics

`performance_monitor` can report:

- total and per-core CPU use;
- Internal, PSRAM, and DMA-capable heap;
- minimum free heap and largest block;
- allocation/free block counts and fragmentation estimate;
- task state, CPU percentage, stack high-water, and stack placement.

Internal and DMA-capable totals overlap and must not be added.

## 11. Common Problems

| Symptom | Checks |
|---|---|
| Blank LCD | Power, CS/DC/RST/BL, SPI host, ST7735 variant |
| Wrong colors | RGB/BGR order and RGB565 byte order |
| SD mount fails | FAT format, wiring, CS, power, inserted before boot |
| Provisioning QR missing | QR support, active session, GUI logs |
| Provisioning receives credentials but no IPv4 | AP credentials, DHCP, signal, grace logs |
| Firebase login fails | Provider enabled, user enabled, menuconfig values, API restrictions |
| Database permission denied | UID and Realtime Database Security Rules |
| TLS/auth retry | Internet, certificate bundle, API restrictions, account state |
| Reset does not trigger | Active-low GPIO9, debounce, full five-second hold |
| Dependencies do not resolve | Activate ESP-IDF and allow Component Manager access |

## 12. Reset Local Build Configuration

Linux/macOS:

```bash
rm -f sdkconfig sdkconfig.old
idf.py reconfigure
```

Windows PowerShell:

```powershell
Remove-Item sdkconfig,sdkconfig.old -ErrorAction SilentlyContinue
idf.py reconfigure
```

Run `idf.py menuconfig` again before building so local Firebase development
configuration is restored.
