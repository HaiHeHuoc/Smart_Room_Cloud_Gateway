# Xiaozhi P2-F Known-Audio HIL Handoff

Updated: 2026-08-25
Base branch: `phase/12.5-transport-validation-clean`
Status: **FIXTURE + SD TEST INFRASTRUCTURE IMPLEMENTED / BUILD + HIL PENDING**

## Goal

Close Phase 12 P2-F with one deterministic known-audio transaction without
using production microphone/speaker ownership.

Known phrase:

`What is two plus two?`

The board-facing fixture is:

`components/application/xiaozhi_foundation/test_assets/p2f_fixture.xzf`

Copy it to the SD card as:

`/xiaozhi/p2f_fixture.xzf`

which mounts on target as:

`/sdcard/xiaozhi/p2f_fixture.xzf`

## Fixture facts

- project-generated locally with eSpeak; no third-party human speech is redistributed;
- source converted to 16 kHz mono;
- Opus VOIP encoding;
- 60 ms packets;
- 33 packets;
- 180 bytes per Opus packet;
- XZF1 total size: 6030 bytes;
- SHA-256: `e3cda4c2dbbfe2ff8acd06f4f1f7cfc1645cfe6d7e0a86c6bcd99d44f1a2b5d3`.

Local fixture verification used libopus
`opus_packet_get_nb_samples(packet, packet_size, 16000)` and every packet
returned 960 samples, proving the intended 60 ms packet duration at 16 kHz.

## Implementation

Validation-only/default-OFF infrastructure was added to the Phase 12 base
branch:

- `CONFIG_XIAOZHI_FOUNDATION_P2F_SD_FIXTURE`;
- SD loader using the existing `sd_card_manager` mounted-VFS lease;
- fixed validation-only 6030-byte linker buffer backing the existing P2-F XZF1 parser;
- bounded 30-second wait for SD readiness;
- exact file-size/header validation before the existing Xiaozhi P2-F worker runs;
- no microphone, speaker, I2S, production `audio_manager`, Wi-Fi ownership, or provisioning ownership changes.

The SD loader is intentionally used only when P2-F E2E validation is selected.
Normal Gateway builds remain default-OFF.

## Expected test flow

```text
boot Gateway
-> SD manager mounts /sdcard
-> network coordinator reaches ONLINE
-> cloud/audio startup reaches the existing Xiaozhi steady-state gate
-> SD fixture loads into validation-only buffer
-> existing P2-F parser validates XZF1 records
-> WebSocket/chat session starts
-> audio channel opens
-> 33 known Opus packets transmit at ~60 ms pacing
-> server STT publishes USER text
-> server response publishes ASSISTANT text
-> server audio callback receives response audio
-> conversation turn completes
-> channel/session cleanup
-> validation summary
```

## Expected log

Timestamps and exact assistant wording may vary. Acceptance requires the
following semantic order/markers:

```text
I (...) XZ_P2F_SD: P2F_SD_FIXTURE result=READY path=/sdcard/xiaozhi/p2f_fixture.xzf bytes=6030 frames=33 frame_ms=60 sample_rate=16000 channels=1
I (...) XIAOZHI_FOUNDATION: === P2-F KNOWN AUDIO E2E ===
I (...) XIAOZHI_FOUNDATION: AUDIO_CHANNEL_OPENED
I (...) XIAOZHI_FOUNDATION: ... frames_sent=33 ...
I (...) XIAOZHI_FOUNDATION: CHAT_TEXT role=USER ...
I (...) XIAOZHI_FOUNDATION: USER text="...two...plus...two..."
I (...) XIAOZHI_FOUNDATION: CHAT_TEXT role=ASSISTANT ...
I (...) XIAOZHI_FOUNDATION: ASSISTANT text="..."
I (...) XIAOZHI_FOUNDATION: Conversation Q/A turn complete
I (...) XIAOZHI_FOUNDATION: ... audio_rx_callback_count=... audio_rx_total_bytes=...
I (...) XIAOZHI_FOUNDATION: VALIDATION SUMMARY checkpoint=P2-F_AUDIO_E2E result=PASS ...
```

The USER transcript may differ in punctuation/case or represent the phrase as
`2 plus 2`; it must still clearly match the known semantic content. Assistant
text must be non-empty. Received server audio and completed-turn evidence are
required by the existing P2-F contract.

## Important failure markers

```text
P2F_SD_FIXTURE result=FAIL reason=sd-not-ready ...
P2F_SD_FIXTURE result=FAIL reason=lease ...
P2F_SD_FIXTURE result=FAIL reason=open ...
P2F_SD_FIXTURE result=FAIL reason=content ...
P2-F response timeout ...
VALIDATION SUMMARY checkpoint=P2-F_AUDIO_E2E result=FAIL ...
```

A failure is evidence to debug, not permission to bypass the public Xiaozhi
flow with private/raw protocol messages.

## Hardware procedure

1. Checkout `test/xiaozhi-p2f-known-audio-e2e`.
2. Ensure the normal project Wi-Fi/Firebase development configuration needed by the Gateway is available locally.
3. Create `xiaozhi/` at the SD-card root.
4. Copy `p2f_fixture.xzf` from the repository into that directory.
5. Insert SD before boot.
6. Clean/reconfigure if an old local `sdkconfig` would override the branch test defaults.
7. Build, flash and monitor from reset.
8. Preserve the complete serial log through P2-F cleanup.

## Acceptance

Do not mark P2-F accepted until target evidence proves in one clean run:

- fixture READY from SD;
- WebSocket connected;
- audio channel opened;
- 33/33 frames transmitted;
- USER transcript semantically matches the known phrase;
- ASSISTANT transcript exists;
- conversation turn complete;
- server audio bytes received;
- channel/session cleanup completes;
- no panic/assert/WDT;
- no obvious resource-retention regression compared with the existing Phase 12 baseline.

## Current verification boundary

Confirmed without target hardware:

- XZF1 file is generated and committed;
- frame count/size/header are deterministic;
- all 33 encoded packets independently report 960 samples at 16 kHz through libopus;
- SD path matches the project `/sdcard` mount contract;
- test remains validation-only/default-OFF in the base Phase 12 branch.

Not confirmed yet:

- ESP-IDF compile/link of the new SD wrapper on the project toolchain;
- real SD read on ESP32-S3;
- Xiaozhi server STT/TTS response to this fixture;
- hardware resource behavior.

Therefore current status is **READY FOR BUILD/HIL**, not hardware PASS.
