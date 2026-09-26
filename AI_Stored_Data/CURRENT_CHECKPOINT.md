# Current Working Checkpoint

Purpose: compact handoff for the active focused fix. Repository/source and
actual target evidence remain authoritative.

Updated: 2026-09-26

## Active work

Branch: `fix/xiaozhi-ptt-uplink-drop`

Base integration branch: `main_including_Firebase_security`

Base HEAD: `18044491f201d9a944aa6cf170f8f85614f7057b`

Current phase: focused Xiaozhi PTT uplink robustness fix before Sprint 24
WakeNet implementation.

Sprint 23 was explicitly accepted by Hải on 2026-09-26. Broader Sprint-23
closure documentation reconciliation on the integration branch remains
separate from this focused code fix.

## User-visible symptom

During a normal GPIO38 Push-To-Talk turn, capture starts successfully but
Xiaozhi can hear the utterance incorrectly: missing words/syllables, dropped
content, or reduced recognition accuracy.

The selected fix intentionally does not wait for a pre-fix hardware baseline.
It hardens the already bounded capture-to-uplink path while preserving current
ownership.

## Implemented changes

### Voice uplink

- `voice_uplink` priority raised from 5 to 6.
- `audio_manager` remains priority 7 and sole I2S/RX/DMA owner.
- Uplink PCM queue increased from 8 to 16 frames.
- At 256 samples/frame and 16 kHz, queue jitter headroom increases from about
  128 ms to about 256 ms.
- Queue storage remains PSRAM-backed.
- The audio-manager stream callback remains non-blocking and still uses
  zero-wait `xQueueSend()`; network latency never blocks the I2S owner.
- Turn summary now records queue peak depth, maximum Opus encode duration, and
  maximum foundation/network send duration for post-fix HIL evidence.

### Recording-critical background policy

Existing behavior was preserved:

- performance monitor defers during live voice recording;
- persistent log writer parks/defers at its safe point;
- ordinary periodic cloud uploads already defer during
  `VOICE_RECORDING_CRITICAL`.

Additional Local Web policy:

- new Storage downloads are rejected while live recording is critical;
- an in-progress Storage download terminates if recording becomes critical;
- new Storage uploads are rejected while recording is critical;
- an in-progress upload is aborted/cleaned if recording becomes critical;
- Diagnostics export is rejected while recording is critical.

No Wi-Fi, TCP/IP, ESP event, IPC, timer, or scheduler suspension was added.

## Commits

- `589bbdae0afcf29773345c80817abadd25fb2b6d`
  `fix(voice): prioritize and buffer live PTT uplink`
- `9c7742d3e53ac823543d50593ec121759b4648c2`
  `fix(web): honor voice recording critical window`
- `609aa0d4d6e34b5656a1021c1302154167728e73`
  `fix(web): defer heavy transfers during PTT capture`

## Validation actually performed

- Pre-fix hardware test: intentionally skipped per Hải's instruction.
- Source/diff review after the fix: PASS.
- Branch comparison against base: three implementation files/areas changed,
  plus this checkpoint.
- Confirmed no global scheduler suspend and no Wi-Fi/TCPIP task suspend.
- ESP-IDF build: NOT RUN in this connector-only session.
- Target/HIL: NOT RUN yet.

## Required post-fix HIL

Flash this branch and perform several PTT turns:

1. Speak immediately after pressing GPIO38.
2. Speak continuously for 5-10 seconds.
3. Repeat while Web dashboard is open.
4. Optionally attempt a Web upload/download during PTT and confirm it is
   deferred/rejected instead of competing with the voice path.

Capture the `VOICE_UPLINK` turn summary. Highest-value fields:

- `queue_drops`
- `stale_drops`
- `queue_peak=<N>/16`
- `max_encode_us`
- `max_send_us`
- `capture_to_first_pcm_ms`
- `capture_to_first_opus_ms`

Also inspect `audio_manager` diagnostics for RX overflow/timeouts.

Expected nominal result:

- queue_drops = 0
- unexpected stale_drops = 0
- RX overflow delta = 0
- RX timeout delta = 0

If queue depth still reaches 16/16 or queue_drops remain non-zero while RX is
clean, the next justified architecture step is to decouple Opus encoding from
the blocking network sender using a second bounded Opus packet queue.

Do not increase I2S DMA descriptors or add another sender task before this
post-fix evidence.

## Scope boundary

- No ESP-SR/WakeNet implementation in this fix branch.
- No new I2S owner.
- No direct provider handle leakage.
- No unrelated architecture changes.
- No merge into `main_including_Firebase_security` without Hải's explicit
  instruction.
