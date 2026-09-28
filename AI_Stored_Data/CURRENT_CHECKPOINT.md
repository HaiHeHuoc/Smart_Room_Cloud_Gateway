# Current Working Checkpoint

Purpose: compact, overwriteable handoff for the active integration state.
Current source, `AGENTS.md`, canonical documentation, and explicit build/HIL
evidence remain authoritative.

Updated: 2026-09-28
Active branch: `main_including_Firebase_security`
Source integration baseline: `44e6feb23f3358917171b6b326b56fdec8ae7ff3`
Baseline commit: `merge: integrate PTT uplink robustness fix`

## Current status

- Sprint 18: COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16.
- Sprint 19: SOURCE INTEGRATED / BUILD VERIFIED / TARGET HIL PARTIAL.
- Sprint 20: IMPLEMENTED / BUILD VERIFIED / TARGET HIL PENDING.
- Sprint 21: COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-24.
- Sprint 22: COMPLETE / BUILD VERIFIED / USER ACCEPTED BY HẢI ON 2026-09-25.
- Sprint 23: COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-26.
- Sprint 24: PLANNED / NOT STARTED on this integration branch.

Sprint-23 source remains integrated. Its former target/browser HIL matrix is
optional regression coverage after Hải's acceptance and must not be used to
rewrite Sprint 23 back to IN PROGRESS.

## Integrated PTT uplink robustness fix

The focused `fix/xiaozhi-ptt-uplink-drop` work is now integrated into
`main_including_Firebase_security` through baseline `44e6feb...`.

Effective source changes retained on the integration branch:

- `voice_uplink` priority is 6, below the priority-7 `audio_manager` I2S owner.
- PCM uplink queue length is 16 frames (~256 ms at 256 samples/frame, 16 kHz).
- Audio capture callback remains zero-wait/non-blocking; network latency does
  not take I2S ownership.
- Per-turn diagnostics include queue peak depth, maximum Opus encode time, and
  maximum provider/network send time in addition to existing queue/drop/timing
  counters.
- Local Web Storage upload/download and Diagnostics export reject or abort at
  safe boundaries while `VOICE_RECORDING_CRITICAL` is active.
- `performance_monitor` defers CPU/task/memory reporting during the recording
  critical window and resumes from a lightweight transition notification.
- No Wi-Fi/TCPIP task suspension, scheduler suspension, new I2S owner, or direct
  provider-handle leakage was introduced.

## Validation state for the robustness fix

- Source/diff inspection: PASS.
- Merge into the requested integration branch: CONFIRMED.
- ESP-IDF build specifically for this robustness merge: NOT RECORDED here.
- Post-fix target/HIL for PTT audio quality and queue behavior: NOT RECORDED.
- Do not inherit earlier build/HIL evidence as proof for these affected paths.

## Required post-fix HIL

Run several GPIO38 PTT turns and capture the `VOICE_UPLINK` turn summary:

1. Speak immediately after pressing PTT.
2. Speak continuously for 5-10 seconds.
3. Repeat while Local Web is open.
4. Optionally attempt Storage upload/download during PTT and verify it is
   rejected/deferred instead of competing with capture.

Highest-value fields:

- `queue_drops`
- `stale_drops`
- `queue_peak=<N>/16`
- `max_encode_us`
- `max_send_us`
- `capture_to_first_pcm_ms`
- `capture_to_first_opus_ms`

Also inspect `audio_manager` RX overflow/timeout diagnostics.

Expected nominal evidence is zero queue drops, zero unexpected stale drops,
zero RX overflow delta, and zero RX timeout delta. If the queue still reaches
16/16 or drops frames while RX remains clean, the next justified architecture
step is a second bounded Opus packet queue between encoding and blocking send;
do not increase DMA descriptors or add another sender task before that evidence.

## Durable boundaries

- Preserve existing manager/service ownership and `application -> service ->
  driver/framework` dependency direction.
- Local Web remains a frontend and does not own Wi-Fi/provisioning, SD/FATFS,
  LVGL, audio/I2S, NeoPixel/RMT/GPIO, or provider lifecycle.
- Never expose credentials, tokens, PoP/session material, private provider
  handles, raw pointers, or unrestricted filesystem content.
- Sprint 24 Wake Word + Advanced Voice UX does not start automatically.

## Next action

Primary gate: run the post-fix PTT HIL above and record only observed evidence.
After that, Sprint 24 may start only when Hải explicitly requests it.
