# P2-F Known-Audio Fixture

## Purpose

This fixture validates the Xiaozhi WebSocket audio/STT/TTS path without using the production microphone or speaker path.

Known spoken phrase:

`What is two plus two?`

The committed board fixture is `p2f_fixture.xzf`. It is an XZF1 packet container, not a WAV/Ogg file. Copy it to the SD card at:

`/xiaozhi/p2f_fixture.xzf`

so the ESP32 VFS path becomes:

`/sdcard/xiaozhi/p2f_fixture.xzf`

## Fixture properties

- project-generated speech using local eSpeak synthesis; no third-party speech sample is redistributed;
- mono speech;
- source resampled to 16 kHz;
- Opus VOIP encoding;
- 60 ms Opus packets;
- 33 packets;
- 180 bytes per packet in the generated fixture;
- XZF1 container size: 6030 bytes;
- SHA-256: `e3cda4c2dbbfe2ff8acd06f4f1f7cfc1645cfe6d7e0a86c6bcd99d44f1a2b5d3`.

Local generation verification used libopus `opus_packet_get_nb_samples(..., 16000)` and confirmed 960 samples for every packet, i.e. exactly 60 ms at 16 kHz.

## Hardware procedure

1. Checkout the dedicated P2-F hardware-test branch.
2. Create directory `xiaozhi` at the SD-card root.
3. Copy `components/application/xiaozhi_foundation/test_assets/p2f_fixture.xzf` to `SD:/xiaozhi/p2f_fixture.xzf`.
4. Insert the SD card before boot.
5. Build/flash the test branch using the branch's test defaults.
6. Start serial monitor from reset and keep the complete log until cleanup finishes.

## Expected log checkpoints

The exact timestamps and assistant wording are server-dependent. The following markers/order are the acceptance contract:

```text
XZ_P2F_SD: P2F_SD_FIXTURE result=READY path=/sdcard/xiaozhi/p2f_fixture.xzf bytes=6030 frames=33 frame_ms=60 sample_rate=16000 channels=1
XIAOZHI_FOUNDATION: === P2-F KNOWN AUDIO E2E ===
XIAOZHI_FOUNDATION: AUDIO_CHANNEL_OPENED
XIAOZHI_FOUNDATION: ... frames_sent=33 ...
XIAOZHI_FOUNDATION: CHAT_TEXT role=USER ...
XIAOZHI_FOUNDATION: USER text="...two...plus...two..."
XIAOZHI_FOUNDATION: CHAT_TEXT role=ASSISTANT ...
XIAOZHI_FOUNDATION: ASSISTANT text="..."
XIAOZHI_FOUNDATION: Conversation Q/A turn complete
XIAOZHI_FOUNDATION: ... audio_rx_callback_count=... audio_rx_total_bytes=...
XIAOZHI_FOUNDATION: VALIDATION SUMMARY checkpoint=P2-F_AUDIO_E2E result=PASS ...
```

The USER transcript does not need byte-for-byte punctuation/case equality, but it must clearly represent the known phrase and include the semantic content `two plus two` (or an equivalent transcription such as `2 plus 2`). The assistant text must be non-empty. The P2-F evidence contract also requires a completed turn and received server audio bytes.

## Failure markers

Missing/not mounted SD:

```text
XZ_P2F_SD: P2F_SD_FIXTURE result=FAIL reason=sd-not-ready ...
```

Wrong path:

```text
XZ_P2F_SD: P2F_SD_FIXTURE result=FAIL reason=open path=/sdcard/xiaozhi/p2f_fixture.xzf
```

Wrong/corrupt fixture:

```text
XZ_P2F_SD: P2F_SD_FIXTURE result=FAIL reason=content ...
```

Server/transport failure or incomplete STT/TTS evidence must end with a P2-F validation failure/timeout rather than being accepted.

## Acceptance rule

Do not mark P2-F hardware accepted until a target run proves all of the following in one transaction:

- SD fixture READY;
- WebSocket connected;
- audio channel opened;
- all 33 packets transmitted;
- USER transcript received and semantically matches the known phrase;
- ASSISTANT transcript received and non-empty;
- conversation turn complete;
- server audio received;
- channel/session cleanup completes;
- no panic, assert, watchdog, or obvious retained-resource regression.

Static/local fixture verification is not hardware acceptance.
