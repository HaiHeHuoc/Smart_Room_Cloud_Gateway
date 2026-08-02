# Setup And Build Guide

## Supported Baseline

- ESP32-S3 N16R8
- ESP-IDF 6.0.1
- 16 MB flash
- 8 MiB Octal PSRAM at 80 MHz
- ST7735 128x160 SPI LCD
- DHT22
- FAT-formatted microSD card
- Active-low factory-reset button

## 1. Install ESP-IDF

Install and activate ESP-IDF 6.0.1 using Espressif's normal installer or command-line setup. Confirm:

```bash
idf.py --version
```

The application manifest requires ESP-IDF 6.0 or newer, but the validated project baseline is 6.0.1.

## 2. Clone And Select The Target

```bash
git clone <repository-url>
cd Smart_Room_Cloud_Gateway
idf.py set-target esp32s3
```

Managed dependencies are resolved from `main/idf_component.yml` during configuration/build.

## 3. Wire The Hardware

### LCD

| ST7735 signal | ESP32-S3 GPIO |
|---|---:|
| MOSI / SDA | 11 |
| SCLK / SCL | 12 |
| CS | 10 |
| DC / A0 | 13 |
| RST / RES | 14 |
| BL / LED | 15 |
| MISO | Not used |

### MicroSD

| SD signal | ESP32-S3 GPIO |
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

Confirm the exact voltage and module pin labels before applying power. The repository pin map is defined in `components/system/common/include/board_config.h`.

## 4. Prepare The SD Card

The current application treats SD mount failure as a startup failure.

- Use a FAT-formatted card.
- Insert it before boot.
- Automatic formatting is disabled to protect card data.
- The current SDSPI clock is intentionally conservative at 2 MHz.

The card mounts at:

```text
/sdcard
```

LVGL exposes it through the registered `S:` drive.

## 5. Configure Firebase Locally

Run:

```bash
idf.py menuconfig
```

Open:

```text
Smart Room Cloud Gateway
└── Firebase development configuration
```

Set:

- Firebase Web API key
- Dedicated device account email
- Dedicated device account password
- Optional expected Firebase UID

The values are written to the local generated `sdkconfig`, which is ignored by Git.

### Firebase Project Requirements

1. Enable Email/Password authentication.
2. Create a dedicated development device account.
3. Enable Realtime Database.
4. Restrict database rules to the intended device identity and path.
5. Verify the URL used in `main/main.c` matches your Firebase project and region.

The current telemetry path is a latest-value endpoint, not historical storage.

### Security Warning

Menuconfig prevents new credentials from being committed in source, but the values are still compiled into development firmware. Do not use an administrator account. Rotate any credential that has ever entered Git history.

## 6. Review Important Configuration

The repository `sdkconfig.defaults` enables:

```text
ESP32-S3 target
16 MB flash
custom partition table
NimBLE
8 MiB Octal PSRAM at 80 MHz
external NimBLE allocation
Wi-Fi/lwIP PSRAM preference
external mbedTLS allocation
external BSS support
LVGL custom allocator and QR code support
FreeRTOS runtime statistics
```

The custom partition table contains:

| Partition | Size |
|---|---:|
| NVS | 24 KiB |
| PHY init | 4 KiB |
| Factory application | 4 MiB |

## 7. Build

```bash
idf.py build
```

For a clean rebuild:

```bash
idf.py fullclean
idf.py build
```

## 8. Flash And Monitor

```bash
idf.py -p <PORT> flash monitor
```

Examples of port names:

```text
Windows: COM5
Linux:   /dev/ttyACM0
macOS:   /dev/cu.usbmodemXXXX
```

Do not copy an example port blindly; use the port assigned to your board.

Exit the ESP-IDF monitor with:

```text
Ctrl+]
```

## 9. First Boot

### Valid Stored Configuration

Expected flow:

```text
boot
-> display/LVGL/SD initialization
-> config validation
-> Wi-Fi connecting
-> IPv4 acquired
-> Wi-Fi status screen
-> sensor dashboard
-> Firebase authentication and upload
```

### No Valid Wi-Fi Configuration

Expected flow:

```text
boot
-> provisioning screen
-> active BLE QR code
-> phone sends credentials
-> Wi-Fi and IPv4 verification
-> NVS save and read-back
-> BLE cleanup
-> Wi-Fi status
-> sensor dashboard
```

Use an Espressif-compatible provisioning client that supports BLE transport and Security 1. The QR code is generated from the active service identity shown by the firmware.

## 10. Functional Checks

### Display

- Correct orientation and colors.
- No clipping on the 160x128 logical layout.
- Provisioning QR is scannable.
- Sensor and cloud states update without flicker or freeze.

### Sensor

- Temperature and humidity update approximately every two seconds.
- Invalid readings enter error/stale handling without crashing the UI.

### Cloud

- Firebase latest-value path changes after a valid sample.
- Wi-Fi loss moves cloud state to wait/retry.
- Network recovery returns cloud state to synchronization/online.

### Factory Reset

1. Hold the button for five seconds.
2. Verify the reset result screen.
3. Verify reboot.
4. Verify the device returns to provisioning.
5. Reprovision without running `erase-flash`.

## 11. Runtime Diagnostics

`performance_monitor` reports:

- CPU use for the system and each core.
- Internal, PSRAM, and DMA-capable heap.
- Minimum free heap and largest block.
- Allocation/free block counts and a fragmentation estimate.
- Task states, CPU percentages, stack high-water marks, and stack locations.

Do not add Internal and DMA totals; their heap capabilities overlap.

## 12. Common Problems

| Symptom | Checks |
|---|---|
| White or blank LCD | Power, CS/DC/RST/BL, SPI host, ST7735 variant |
| Wrong colors | RGB/BGR order and RGB565 byte order |
| SD mount fails | Card format, wiring, CS, power, inserted before boot |
| Provisioning QR missing | QR support in sdkconfig, active provisioning session, GUI logs |
| Provisioning connects but no IPv4 | AP credentials, DHCP, signal, session grace logs |
| Firebase init fails | All menuconfig fields are set and endpoint matches project |
| TLS/auth retry | Internet, time/cert bundle, account permission, database rules |
| Reset does not trigger | Active-low GPIO 9, debounce, full five-second press |
| Build cannot find dependencies | Activate ESP-IDF and allow Component Manager resolution |

## 13. Clean Local Configuration

To reset local configuration without changing repository defaults:

```bash
rm -f sdkconfig sdkconfig.old
idf.py reconfigure
```

On Windows PowerShell:

```powershell
Remove-Item sdkconfig,sdkconfig.old -ErrorAction SilentlyContinue
idf.py reconfigure
```

Run `idf.py menuconfig` again before building so Firebase development configuration is restored locally.
