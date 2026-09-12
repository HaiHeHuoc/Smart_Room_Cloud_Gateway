# Next Work + Deferred HIL Backlog

Updated: 2026-09-13
Active branch: `main_including_Firebase_security`
Observed production/source HEAD before AI metadata sync: `15cd0f06d25142a6ed7672bc99dfd4ec396184b0`

Purpose: cross-session routing for "hiện tại nên làm gì tiếp theo?" and deferred
acceptance work. This file is not a substitute for current source, `AGENTS.md`,
or actual hardware/build evidence.

## Current software state

```text
Phase 12     COMPLETE / HIL PASS
Phase 13     COMPLETE / HIL PASS
Phase 14     SOFTWARE COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Phase 15     COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Phase 16     COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1   IMPLEMENTED / BUILD VERIFIED / automated HIL PASS / audible recovery accepted / endurance pending
Phase 17     COMPLETE / BUILD VERIFIED / read-only MCP voice HIL accepted
Phase 18     IN PROGRESS
Phase 18.1   COMPLETE / BUILD PASS / target HIL accepted by user
Phase 18.2   NOT STARTED
Phase 18.3   NOT STARTED
Phase 18.4   NOT STARTED
Phase 19     NOT STARTED
```

Phase 18.1 is complete. See
`AI_Stored_Data/PHASE18_MCP_CONTROLLED_ACTIONS.md` for the exact current light
contract and validation matrix.

## Immediate next work

The highest-value next step is the deferred PTT/TLS and streaming regression,
not starting 18.2.

Recommended order:

```text
1. Boot target and run repeated PTT smoke/regression.
2. Verify current TLS-in-PSRAM headroom behavior.
3. Exercise delayed-first-PCM / streaming path touched by current HEAD.
4. Record evidence.
5. Start 18.2 only when explicitly requested.
```

Why: the current source checkpoint was rebuilt and Phase-18.1 light HIL was
accepted by the user on 2026-09-13. The remaining work is the independent
PTT/TLS and streaming regression affected by the same source checkpoint.

## Current-head regression focus

### TLS / PTT

Current source uses dynamic TLS buffers in PSRAM, 1 KiB outbound TLS records,
and a 20 KiB total/largest-contiguous PSRAM gate before a PTT turn starts.
Validate:

```text
boot cleanly
first PTT
multiple repeated PTT turns
no ESP_ERR_NO_MEM at first WebSocket audio write
expected PSRAM headroom logs
no new I2S/DMA regression
```

### Streaming downlink

Current source uses a 0.96-second normal PCM prefill. The 5-second prefill wait
starts only after the first PCM packet, not at TTS_START. Validate at least one
reply where server/tool/synthesis delay precedes first audio and confirm there
is no false prefill timeout.

### Phase 18.1 light — accepted

The target matrix was accepted by the user on 2026-09-13:

```text
pink 100%
green 20%
brightness 0 / 100
off
rapid updates
solid / blink / breath / pulse / rainbow
effect-only while off -> turns on
effect-only with preserved black RGB -> visible white fallback
off + color -> rejected
off + brightness -> rejected
off + effect -> rejected
light.get_state matches applied logical state
light.get_capabilities matches current fixed allowlist
```

Current pulse timing is 1200 ms. Do not use the older 300 ms expectation.

## Global historical HIL routing

Older accepted phases remain regression baselines:

```text
RUN PHASE 12 HIL   -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL   -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL   -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL   -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL   -> test/phase16-audio-arbitration-hil
RUN PHASE 16.1 HIL -> phase/16.1-streaming-downlink
```

Inspect the actual worktree before routing. Never auto-stash, reset, discard,
delete, or run an old HIL matrix against an arbitrary production branch.

## Historical acceptance notes

### Phase 14

Keep the recorded golden-path PTT/audio response evidence. Deferred
fault-injection/targeted cases should be rerun only when relevant.

### Phase 15

Closed by user acceptance. Preserve it as a regression baseline rather than a
current blocker.

### Phase 16

Bounded target arbitration matrix is accepted. Long-duration/full-Gateway
regression remains deferred.

### Phase 16.1

Automated target streaming matrix and audible recovery are accepted. Endurance
remains pending; current HEAD additionally needs a focused regression because
prefill timing semantics changed after the accepted baseline.

### Phase 17

Read-only MCP sensor, cloud-sync, and system-status slices are closed and voice
HIL accepted. Do not add more read-only tools automatically.

## Broader deferred integration work

After current-head revalidation and Phase-18.1 acceptance, keep these visible:

1. Long-duration Firebase/cloud + Xiaozhi simultaneous traffic.
2. Repeated PTT/endurance and heap/stack/resource trend checks.
3. Notification/alarm repeated timing and preemption endurance.
4. Relevant Phase-15 visible UI/text regressions when defects touch that path.
5. Full post-portability target smoke/regression; architecture itself remains
   frozen unless a concrete defect is found.
6. Release/documentation/portfolio closure after feature and acceptance work.

## Production-vs-test fix policy

```text
Test harness/config/expected-log defect
-> fix on the test branch

Production component/architecture defect
-> fix on the owning production branch first
-> propagate forward to affected test branches
-> rebuild and retest the affected acceptance case
```

Never merge test-harness history as production feature history.

## Evidence vocabulary

- `IMPLEMENTED` — source exists.
- `STATIC REVIEW COMPLETE` — source review performed.
- `BUILD VERIFIED` — an actual relevant ESP-IDF build passed.
- `HIL PASS` — target evidence satisfies the named contract.
- `TARGETED HIL PARTIAL` — only named runtime cases have evidence.
- `PENDING` — no current evidence yet.

A build PASS on an earlier commit does not automatically verify a later source
HEAD that changed the affected code.
