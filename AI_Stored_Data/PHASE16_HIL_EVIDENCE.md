# Phase 16 HIL Evidence — Audio Arbitration

Updated: 2026-09-05
HIL branch: `test/phase16-audio-arbitration-hil`
Target: ESP32-S3 on COM4

## Scope and evidence boundary

This record combines the earlier operator-confirmed real Xiaozhi PTT/speaker
turn with the 2026-09-04 no-GPIO arbitration run. The automated coordinator is
test-only and deliberately does not replace real microphone speech, remote TTS,
or audible-speaker acceptance.

## Build and flash

- `idf.py build` passed with ESP-IDF 6.0.1.
- Application binary: `0x221b30`; smallest app partition free space: 47%.
- The resulting test image booted on the target with the Phase-16 coordinator
  enabled only through the local test configuration.

## Final default-off build

After capturing target evidence, the local Phase-16 test gate was restored to
its default-off state. A normal `idf.py build` then passed with ESP-IDF 6.0.1
(binary `0x21e7b0`, 47% smallest-app-partition free). This final image was not
flashed; the board remains on the last test image until a later explicit flash.

## Target matrix

| Case | Result | Evidence |
|---|---|---|
| T16-01 build/link | PASS | Host `idf.py build` completed successfully. |
| T16-02 boot/startup | PASS | Audio manager and both arbiters IDLE; GUI left boot; voice assistant and Xiaozhi session reached READY. |
| T16-03 Xiaozhi capture | PASS | Earlier real PTT HIL accepted by the operator; this automatic run intentionally did not press GPIO38. |
| T16-04 Xiaozhi playback | PASS | Earlier real Xiaozhi response/speaker HIL accepted by the operator; this automatic run intentionally did not replace audible-speaker evidence. |
| T16-05 notification queue | PASS | Synthetic Xiaozhi playback remained current while notification occupied the bounded pending slot; no preemption. |
| T16-06 alarm preempt | PASS | Interruptible Xiaozhi playback stopped cooperatively and priority-100 alarm became active. |
| T16-07 equal priority | PASS | Equal-priority PREEMPT request was rejected without preemption. |
| T16-08 queue pressure | PASS | Current plus pending were retained and the third request was rejected deterministically. |
| T16-09 capture contention | PASS | Equal requester was rejected; higher known SYSTEM request preempted and became active; no second RX owner. |
| T16-10 capture/playback race | PASS | Back-to-back submission gap was 35 us; playback owned I2S first, capture observed `ESP_ERR_INVALID_STATE`, then became active after playback cancellation. No duplicate I2S owner was observed. |
| T16-11 cancellation | PASS | Pending-before-start, pending, and active cancellation all returned to clean IDLE ownership. |
| T16-12 Gateway coexistence | PASS | Sensor progress advanced; cloud status remained observable/non-UNINITIALIZED; SD/voice/UI state snapshots were valid; both arbiters returned clean and no playback/capture failures were recorded. This bounded window did not prove cloud upload progress or an LVGL-task heartbeat. |

Automatic summary: `pass=10 fail=0 skip=2`. The two automatic `SKIP` cases
were T16-03 and T16-04 only; their real PTT/speaker acceptance comes from the
operator-confirmed manual HIL above. A short post-summary monitor window showed
no panic, assertion, WDT, or boot loop.

The T16-10 harness accepts both valid loser paths: retry after
`ESP_ERR_INVALID_STATE`, or retained logical ownership while the opposite
resource is already active. A preliminary rerun exposed that the latter path
was incorrectly classified as a failure; the test-only expectation was fixed
and the final target matrix above is from that corrected source.

Sanitized target markers retained from the run:

```text
PH16_TEST T16_10 submit_delta_us=35 capture_ret=ESP_OK playback_ret=ESP_OK
PH16_TEST T16_10 winner=PLAYBACK loser=CAPTURE wait=RETRY_AFTER_INVALID_STATE
PH16_TEST T16_10 PASS
PH16_TEST T16_12 PASS
PH16_TEST SUMMARY pass=10 fail=0 skip=2
```

## Closure limitations retained

- This is a bounded arbitration/coexistence run, not a long-duration endurance
  or memory-leak campaign.
- It does not prove every possible multi-task timing interleaving.
- Streaming Xiaozhi playback is intentionally outside Phase 16; the accepted
  SD/WAV downlink path remains unchanged.
