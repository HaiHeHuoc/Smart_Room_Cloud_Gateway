# P2-F Validation Fixture

`p2f_fixture.bin` is intentionally absent from the repository. Before enabling
the P2-F Kconfig options, place a legal, non-sensitive, user-created fixture at
this exact path.

It is a test-only `XZF1` binary container, not PCM, WAV, Ogg, WebM, or an audio
file that a decoder must parse. Every frame record contains one already encoded
Opus packet for the spoken test sentence (for example, a user-recorded
"What is two plus two?"). The server-facing P2-F contract is fixed to Opus,
16 kHz, mono, 60 ms packets.

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
| 16 | 4 | number of frame records (1..120) |
| 20 | 4 | total bytes in the frame-record region |
| 24 | variable | repeated records: `uint16_t packet_size`, then one raw Opus packet |

Each packet must be non-empty, at most 2048 bytes, and represent exactly 60 ms
of the same 16 kHz mono speech stream. The total container must not exceed
64 KiB. P2-F transmits one record per `esp_xiaozhi_chat_send_audio_data()` call
and delays approximately 60 ms between records. It neither decodes nor plays
the received server audio. The parser can validate the container declaration
and record bounds but cannot decode Opus to prove an individual packet duration.

Do not commit copyrighted third-party speech samples, credentials, or raw
binary logs. Keep the fixture local unless its licensing and privacy status are
explicitly cleared.
