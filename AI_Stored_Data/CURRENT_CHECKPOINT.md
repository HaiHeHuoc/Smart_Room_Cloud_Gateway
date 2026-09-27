# Current Working Checkpoint

Purpose: compact handoff for the isolated Sprint-24 rework. Repository source
and actual target evidence remain authoritative.

Updated: 2026-09-27

## Active work

Branch: `rework/24-wake-word`

Base integration branch: `main_including_Firebase_security`

Base/HEAD: `44e6feb23f3358917171b6b326b56fdec8ae7ff3`

Current scope: Phase 24.2.2 WakeNet + observational VAD over the accepted
continuous AFE baseline. The failed prior Sprint-24 implementation was
inspected only as negative evidence and was not merged, rebased, or
cherry-picked.

## Accepted 24.2.1 baseline

- `audio_manager` remains the sole I2S RX/DMA owner.
- Its dedicated local-monitor mode copies selected mono PCM16 frames through
  an independent observer; the existing PTT uplink observer is unchanged.
- `afe_pipeline` owns only a four-frame, Internal-RAM PCM queue plus one feed
  and one fetch worker (priority 5, unpinned, 6144-byte Internal stacks).
- The ESP-SR dependency is pinned to `espressif/esp-sr` 2.4.7.
- Hải directly confirmed the AFE overlay was flashed, ran normally, and had no
  observed error. No detailed serial-counter capture was supplied in this
  handoff; this confirmation authorizes the next gated layer but does not
  replace Phase-24.2.2's separate HIL evidence.
- Feed retries only a documented zero/rejected result while the independent
  fetch worker continues to drain. Negative feed results are counted without
  reset/rearm recovery. Fetch runs continuously using ESP-SR's default API.
- Startup and 60-second aggregate serial logs expose producer/consumer,
  queue, stack-HWM, heap, task-count, and copied CPU snapshots. The same
  bounded data is available from `afe_pipeline_get_status()`.

## Implemented 24.2.2

- The enabled overlay selects and packs only `wn9_hiesp` in a new 512 KiB
  `model` partition. Startup opens the packed list, verifies that exact model,
  then creates the existing 16 kHz mono AFE with WakeNet and observational
  WebRTC VAD enabled. AEC, SE, NS, and AGC remain disabled.
- AFE fetch remains the continuous consumer. It only copies scalar wake
  metadata to a four-event Internal-RAM queue with zero wait; a full queue is
  counted/dropped and cannot delay fetch or feed.
- A low-priority detection worker owns bounded detection logging. It performs
  no network, PTT, I2S arbitration, filesystem, cloud, or LVGL operation.
- Status adds wake/event counters, VAD speech/non-speech/transition counters,
  last wake metadata, event-queue depth/peak, and the detection-worker HWM.

## Explicitly excluded

- Wake-to-Xiaozhi handoff; virtual PTT; advanced conversation UX; GUI changes;
  speaker/playback interaction; suppression, lifecycle/rearm, and automatic
  AFE reset.
- Feature-off remains the default via `CONFIG_AFE_PIPELINE_ENABLE=n`, retaining
  normal Sprint 0-23 behavior and GPIO38 PTT baseline.

## Validation actually performed

- Prior 24.2.1 checks remain: `git diff --check`, audio-manager host tests,
  voice-assistant host tests, and ESP-IDF 6.0.1 enabled build passed.
- Phase-24.2.2 ESP-IDF 6.0.1 enabled build: PASS after model reconfigure.
  `Smart_Room_Cloud_Gateway.bin` is 3,063,248 bytes, a 3,120-byte delta from
  the prior enabled build; the 4 MiB app partition retains 1,131,056 bytes
  (27%) free. `srmodels.bin` is 291,142 bytes and contains only `wn9_hiesp`.
- On this Windows host, set `PYTHONIOENCODING=utf-8` before invoking raw Ninja:
  ESP-SR's model-pack report prints Unicode and otherwise fails under cp1252.
- Target/HIL: 24.2.2 NOT RUN. Build evidence does not establish wake quality,
  continuous timing, memory trend, or coexistence.

## Required HIL gate before Phase 24.3

1. Flash both the app and `model` partition from `build_afe`; confirm one AFE
   proof-start line and one WakeNet/VAD-start line naming `wn9_hiesp`.
2. Idle 10-15 minutes and retain boot plus two 60-second summaries. Feed/fetch
   must advance with zero nominal PCM/event drops, no persistent full/empty
   warnings, reset, watchdog, crash, or recovery loop.
3. Say `Hi ESP` 20-50 times, including short-gap repeats. Record attempts,
   detections, misses, false detections, wake event counters, and VAD results.
   Repeated detections must not interrupt fetch/feed.
4. Compare boot, 5-minute, 10-15-minute, and post-detection resource/HWM/task
   snapshots. No monotonic leak is acceptable.
5. FAIL stops later Sprint-24 lifecycle/Xiaozhi work; retain serial evidence
   and analyze this layer only.

## Git boundary

- Changes remain uncommitted. No push, merge, rebase, reset, or PR occurred.
- Do not start Phase 24.3, Xiaozhi integration, virtual PTT, or lifecycle
  work until Hải provides Phase-24.2.2 target HIL PASS evidence.
