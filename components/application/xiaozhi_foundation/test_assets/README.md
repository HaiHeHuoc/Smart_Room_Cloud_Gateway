# P2-F Validation Fixture

The repository now contains one project-generated known-audio fixture for the
Phase 12 P2-F hardware test:

`p2f_fixture.xzf`

The known spoken phrase is:

`What is two plus two?`

The fixture was synthesized locally with eSpeak, resampled to 16 kHz mono,
encoded as Opus VOIP packets with 60 ms frame duration, then packed into the
project XZF1 container. No third-party human speech recording is redistributed.

## SD-card test path

For the dedicated P2-F SD validation branch, copy the committed fixture to:

`SD:/xiaozhi/p2f_fixture.xzf`

The firmware accesses it as:

`/sdcard/xiaozhi/p2f_fixture.xzf`

See `P2F_KNOWN_AUDIO_NOTE.md` for exact hardware steps, expected logs, failure
markers, and acceptance criteria.

## XZF1 format

`p2f_fixture.xzf` is a test-only raw-Opus packet container. It is not PCM, WAV,
Ogg, or WebM. Every frame record contains one already encoded Opus packet.
The server-facing contract is Opus, 16 kHz, mono, 60 ms packets.

All multi-byte fields are little-endian:

| Offset | Size | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `XZF1` |
| 4 | 1 | version `1` |
| 5 | 1 | codec `1` (Opus) |
| 6 | 2 | header size `24` |
| 8 | 4 | sample rate `16000` |
| 12 | 1 | channels `1` |
| 13 | 1 | reserved `0` |
| 14 | 2 | frame duration `60` |
| 16 | 4 | number of frame records |
| 20 | 4 | total bytes in the frame-record region |
| 24 | variable | repeated records: `uint16_t packet_size`, then one raw Opus packet |

Committed fixture facts:

- frame count: 33;
- packet size: 180 bytes each;
- container size: 6030 bytes;
- SHA-256: `e3cda4c2dbbfe2ff8acd06f4f1f7cfc1645cfe6d7e0a86c6bcd99d44f1a2b5d3`.

Local generation verification called libopus
`opus_packet_get_nb_samples(packet, size, 16000)` for every packet and verified
960 samples per packet, which is exactly 60 ms at 16 kHz.

## Legacy embedded mode

The old `CONFIG_XIAOZHI_FOUNDATION_P2F_EMBED_FIXTURE` path remains available
for a deliberately embedded local `p2f_fixture.bin`. The new SD mode is
preferred for the dedicated hardware test because it lets the board load the
fixture through the existing `sd_card_manager` VFS lease without rebuilding the
firmware merely to replace an SD test asset.

Do not replace the fixture with arbitrary PCM/WAV/Ogg bytes. P2-F sends each
XZF1 record directly through `esp_xiaozhi_chat_send_audio_data()` and paces
records at approximately 60 ms.
