# ESP32-S3 N16R8 — Available GPIO Reference

## Purpose

This document lists GPIOs that remain available for connecting additional
peripherals or modules to the current Smart Room Cloud Gateway hardware.

It intentionally excludes GPIOs already assigned in:

```text
components/system/common/include/board_config.h
```

Target assumptions:

- ESP32-S3 N16R8;
- 16 MB flash and 8 MB Octal PSRAM;
- ESP32-S3-DevKitC-1-compatible header layout.

Always compare this reference with the exact board schematic and silkscreen
before wiring a module.

---

## 1. Available GPIOs

### Preferred general-purpose GPIOs

Use these first for additional peripherals:

```text
GPIO1, GPIO5, GPIO6
```

They are free in the current project and do not normally carry boot-strapping,
native USB, default UART0, or JTAG duties.

### Available GPIOs with special considerations

```text
GPIO0, GPIO3,
GPIO19, GPIO20,
GPIO39, GPIO40, GPIO41, GPIO42,
GPIO43, GPIO44,
GPIO45, GPIO46
```

These pins remain usable, but their special roles are described later in this
document.

### Complete available set

```text
GPIO0, GPIO1, GPIO3,
GPIO5, GPIO6,
GPIO19, GPIO20,
GPIO39, GPIO40, GPIO41, GPIO42,
GPIO43, GPIO44, GPIO45, GPIO46
```

---

## 2. Capability Groups

ESP32-S3 uses an IO MUX and GPIO Matrix. Most digital peripheral signals can be
routed to multiple GPIOs instead of requiring one universal fixed pin pair.

## 2.1 General digital GPIO and interrupts

All GPIOs in the complete available set can be used for ordinary digital input,
digital output, or GPIO interrupts.

Preferred group:

```text
GPIO1, GPIO5, GPIO6
```

Typical uses:

- module enable, reset, chip-select, busy, ready, or interrupt signals;
- buttons and status LEDs;
- transistor, relay-driver, or peripheral-control inputs.

## 2.2 ADC

### ADC1 — preferred while Wi-Fi is active

| GPIO | Channel | Recommendation |
|---:|---|---|
| GPIO1 | ADC1_CH0 | Preferred |
| GPIO3 | ADC1_CH2 | Strapping pin; use carefully |
| GPIO5 | ADC1_CH4 | Preferred |
| GPIO6 | ADC1_CH5 | Preferred |

Recommended ADC inputs:

```text
GPIO1, GPIO5, GPIO6
```

### ADC2

| GPIO | Channel | Additional role |
|---:|---|---|
| GPIO19 | ADC2_CH8 | Native USB D- |
| GPIO20 | ADC2_CH9 | Native USB D+ |

Prefer ADC1 for this Wi-Fi-based project. ADC2 has Wi-Fi and continuous-mode
restrictions and GPIO19/GPIO20 are also the native USB differential pair.

## 2.3 Capacitive touch

Available touch-capable pins:

```text
GPIO1, GPIO3, GPIO5, GPIO6
```

Preferred touch pins:

```text
GPIO1, GPIO5, GPIO6
```

GPIO3 is a strapping pin and should not be externally biased to an unintended
level during reset.

## 2.4 RTC GPIO and deep-sleep wake

Available RTC-capable GPIOs:

```text
GPIO0, GPIO1, GPIO3,
GPIO5, GPIO6,
GPIO19, GPIO20
```

Preferred low-conflict RTC pins:

```text
GPIO1, GPIO5, GPIO6
```

Confirm that the selected ESP-IDF wake-up API supports the chosen pin and wake
mode.

## 2.5 I2C

I2C SDA and SCL can be routed through the GPIO Matrix.

Preferred pin pool:

```text
GPIO1, GPIO5, GPIO6
```

Choose any two different suitable pins from that pool.

Electrical notes:

- SDA and SCL are open-drain signals;
- use suitable external pull-ups to 3.3 V;
- verify that every connected module is 3.3 V logic compatible.

## 2.6 I2S and digital audio

I2S BCLK, WS/LRCLK, DIN, DOUT, and optional MCLK can be routed through the GPIO
Matrix.

Preferred pin pool:

```text
GPIO1, GPIO5, GPIO6
```

Typical signal groups:

| Device path | Required signals |
|---|---|
| I2S microphone RX | BCLK, WS/LRCLK, DIN, optional MCLK |
| I2S amplifier TX | BCLK, WS/LRCLK, DOUT, optional MCLK |
| Shared RX/TX clocks | Shared BCLK and WS when supported by the hardware design |

Planning rules:

- keep I2S DMA buffers and descriptors in internal DMA-capable RAM;
- use PSRAM only for larger PCM rings after measurement;
- avoid strapping pins for audio clocks;
- avoid GPIO19/GPIO20 when native USB is needed;
- avoid GPIO43/GPIO44 when the UART0 console is needed.

## 2.7 SPI and SDSPI

SPI signals can be routed through the GPIO Matrix.

Preferred pin pool for a separately routed bus:

```text
GPIO1, GPIO5, GPIO6
```

Possible signals:

```text
SCLK, MOSI, optional MISO, CS,
optional DC, RESET, IRQ, BUSY, READY
```

When sharing a bus:

- give each device a unique CS;
- confirm compatible SPI mode and clock limits;
- serialize access through the owning SPI host or component;
- verify that every inactive device releases MISO.

## 2.8 UART

UART TX, RX, RTS, and CTS can be routed through the GPIO Matrix.

Preferred pin pool for an additional UART:

```text
GPIO1, GPIO5, GPIO6
```

GPIO43 and GPIO44 are normally UART0 console pins on DevKitC-style boards, so
reuse them only when the console and USB-to-UART bridge interaction are
intentionally handled.

## 2.9 PWM, LEDC, MCPWM, RMT, PCNT, and TWAI

These peripherals can use GPIO Matrix routing.

Preferred pin pool:

```text
GPIO1, GPIO5, GPIO6
```

Typical uses:

- LEDC for LED brightness or simple servo pulses;
- MCPWM for motors, complementary PWM, or capture;
- RMT for WS2812, infrared, or precisely timed digital protocols;
- PCNT for pulse, encoder, or frequency counting;
- TWAI for CAN communication through an external CAN transceiver.

Do not connect ESP32-S3 GPIOs directly to a CAN bus.

## 2.10 SDMMC and parallel digital interfaces

SDMMC and other parallel digital interfaces can use GPIO Matrix routing but
consume several pins.

Start from the preferred pool:

```text
GPIO1, GPIO5, GPIO6
```

Before extending into the special-pin group, review USB, JTAG, UART, boot, and
onboard-device conflicts.

---

## 3. Special GPIOs

## 3.1 Strapping and boot pins

```text
GPIO0, GPIO3, GPIO45, GPIO46
```

These pins are sampled during reset and become ordinary GPIOs afterward.

| GPIO | Special consideration |
|---:|---|
| GPIO0 | Boot/download selection; often connected to the BOOT button |
| GPIO3 | Strapping configuration |
| GPIO45 | Strapping configuration |
| GPIO46 | Boot-mode strapping together with GPIO0 |

Only use them when the attached module remains high impedance during reset or
its pull resistors have been verified against the required boot state.

## 3.2 Native USB pins

```text
GPIO19 -> USB D-
GPIO20 -> USB D+
```

They can be reassigned, but doing so conflicts with native USB Serial/JTAG or
USB OTG. Reserve both when USB flashing, debugging, CDC, HID, MSC, or another
USB function is required.

## 3.3 JTAG pins

```text
GPIO39 -> MTCK
GPIO40 -> MTDO
GPIO41 -> MTDI
GPIO42 -> MTMS
```

They can be used as ordinary GPIOs when external JTAG is not required. Reserve
the complete group when using an external JTAG probe.

## 3.4 Default UART0 pins

```text
GPIO43 -> UART0 TX
GPIO44 -> UART0 RX
```

They are commonly connected to the onboard USB-to-UART bridge. Reassigning
them may remove normal serial logging or cause contention with the bridge.

## 3.5 Current PTT and onboard NeoPixel pins

The current board assignment is:

```text
GPIO38 -> dedicated active-high PTT input
GPIO48 -> onboard NeoPixel LED
```

Both are therefore unavailable for additional peripherals.

## 3.6 Flash and Octal PSRAM pins

The N16R8 target uses external flash and 8 MB Octal PSRAM. Pins occupied by the
flash/PSRAM interface must not be reassigned.

For the common ESP32-S3 N16R8 DevKitC-compatible module, treat:

```text
GPIO35, GPIO36, GPIO37
```

as unavailable for external peripherals. They are intentionally absent from
the available GPIO sets in this document.

---

## 4. Quick Selection Table

| Peripheral | First GPIO group to consider | Main caution |
|---|---|---|
| Digital input/output | GPIO1, GPIO5, GPIO6 | Check voltage and current |
| Analog sensor | GPIO1, GPIO5, GPIO6 | Prefer ADC1 with Wi-Fi |
| Capacitive touch | GPIO1, GPIO5, GPIO6 | Validate noise and electrode layout |
| I2C sensor | Any two preferred GPIOs | Add external pull-ups |
| I2S microphone/amplifier | BCLK, WS, DIN/DOUT and optional MCLK from preferred pool | Preserve DMA/internal-RAM headroom |
| SPI module | SCLK, MOSI, optional MISO and CS from preferred pool | Review bus sharing |
| UART module | TX/RX from preferred pool | Avoid GPIO43/GPIO44 unless console is moved |
| Native USB | GPIO19 and GPIO20 | Fixed differential pair |
| External JTAG | GPIO39 through GPIO42 | Reserve the complete group |
| Deep-sleep wake | GPIO1, GPIO5, GPIO6 | Verify wake API support |

---

## 5. Electrical Rules

- ESP32-S3 GPIO logic is 3.3 V; do not apply 5 V directly.
- Use drivers, transistors, amplifiers, or isolation for motors, relays,
  speakers, and other high-current loads.
- Connect grounds between the ESP32-S3 and every externally powered module.
- Verify startup behavior for anything attached to strapping pins.
- Keep high-speed SPI, I2S, USB, and clock wiring short and well grounded.
- Add protocol-required pull-up or pull-down resistors.
- Update `board_config.h` before production code takes ownership of a new pin.

---

## 6. Maintenance Rule

Whenever `board_config.h` receives a new GPIO assignment:

1. remove that GPIO from all available groups in this document;
2. retain its special-function note when relevant;
3. recheck boot, flash/PSRAM, USB, JTAG, UART, and onboard-device conflicts;
4. update the related component documentation in the same change.

---

## 7. Official References

- [ESP32-S3 GPIO and RTC GPIO — ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/peripherals/gpio.html)
- [ESP32-S3-DevKitC-1 User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/)
- [ESP32-S3 Hardware Design Guidelines](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html)
- [ESP32-S3 ADC Driver Documentation](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/peripherals/adc/index.html)
