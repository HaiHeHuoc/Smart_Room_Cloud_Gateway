# Phase 16 Audio Arbitration HIL Test Branch

Updated: 2026-09-05
Test branch: `test/phase16-audio-arbitration-hil`
Base/production branch: `phase/16-audio-arbitration`
Activation command: `RUN PHASE 16 HIL`
Status: **TEST BRANCH SYNCHRONIZED / BUILD VERIFIED / BOUNDED HIL ACCEPTED**

## Purpose

This is the dedicated Phase-16 board-acceptance branch. Production architecture/fixes belong on `phase/16-audio-arbitration`; test-only instrumentation, deterministic stimulus, expected logs and acceptance evidence belong here.

This branch is synchronized through production checkpoint `7faeed8` and its
test merge `de39dfb` passed `idf.py build` with ESP-IDF 6.0.1 on 2026-09-02
(binary `0x21e7b0`, 47% app partition free). This is host build evidence only.

Read `AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md` together with this file.

## Codex command contract

When the user enters:

```text
RUN PHASE 16 HIL
```

Codex must execute the following workflow.

### 1. Route to the correct branch

- inspect current branch and `git status` first;
- if uncommitted/untracked work could be lost, STOP and report it; do not auto-stash/reset/delete;
- checkout `test/phase16-audio-arbitration-hil`;
- verify HEAD really belongs to this branch before modifying/building/flashing.

### 2. Read authoritative test documents

Read at minimum:

```text
Agent.md
AI_Stored_Data/PROJECT_STATE.md
AI_Stored_Data/PHASE16_PROGRESS.md
AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md
AI_Stored_Data/PHASE16_HIL_TEST_BRANCH.md
AI_Stored_Data/NEXT_WORK_AND_HIL_BACKLOG.md
```

If documents conflict, preserve evidence and report the conflict instead of inventing a new contract.

### 3. Inspect before editing

Verify:

- `audio_manager` remains the only I2S/DMA owner;
- playback/capture arbiter source is present in build graph;
- Xiaozhi source-local arbitration bridges are present;
- notification/alarm helper APIs are present;
- Phase-12 validation mode is not accidentally taking production ownership;
- board configuration still matches connected hardware.

Do not rewrite production architecture merely to make a test pass.

### 4. Build

Use the repository's normal ESP-IDF workflow. Prefer a clean/reconfigure build when branch/config changed.

Minimum host-build evidence:

```text
idf.py build -> Project build complete
```

If build fails:

```text
idf.py build -> nonzero exit and the short build failure reason
```

Stop before flash and classify T16-01 FAIL.

### 5. Flash + monitor from reset

Flash the connected ESP32-S3 and capture complete serial output from reset through production readiness.

Expected semantic startup sequence:

```text
audio_manager READY
Phase-16 playback arbiter READY
Phase-16 capture arbiter READY
Phase-15 voice stack READY
PH16_TEST T16_02 PASS
```

Exact ESP_LOG formatting/tag text may differ; judge the semantic state transition, not whitespace.

The board must leave the GUI Starting state. Panic/assert/WDT/boot loop is immediate failure evidence.

### 6. Execute acceptance cases independently

Run T16-01 through T16-12 from `PHASE16_HIL_TEST_PLAN.md`.

Codex may add TEST-BRANCH-ONLY instrumentation/stimulus helpers when necessary. Keep them bounded and clearly prefixed/named as Phase-16 test code. Do not change production policy simply to manufacture PASS evidence.

Suggested machine-readable case markers:

```text
PH16_TEST T16_01 BEGIN
PH16_TEST T16_01 PASS
PH16_TEST T16_01 FAIL reason=<reason>
PH16_TEST T16_01 SKIP reason=<reason>
```

Use the same format for T16_02 ... T16_12.

### 6a. Default-off automated arbitration coordinator

`main/phase16_auto_hil_test.c` is a one-shot, test-branch-only coordinator.
It begins only after the production audio manager has started and waits for
the voice-assistant/Xiaozhi session to become READY before running the
arbitration cases. It uses public capture/playback-arbiter APIs and copied
status snapshots only; it never owns GPIO/PTT, I2S, DMA, WAV file handles, or
raw audio buffers.

If T16-02 cannot establish the voice/Xiaozhi READY precondition, the
coordinator records the later synthetic cases as `SKIP` and stops rather than
creating arbitration-only PASS evidence.

Enable it locally through:

```text
Smart Room Cloud Gateway
-> Phase 16 automated HIL test (test branch only)
-> Run bounded Phase-16 automatic arbitration HIL
```

Set `APP_PHASE16_AUTO_HIL_WAV_PATH` to an existing managed-SD PCM16 mono
16-kHz WAV. The default is `/sdcard/audio/input_long.wav`. Reconfigure,
build, flash, and monitor from reset. The Kconfig default remains `n` and the
test is mutually exclusive with the older continuous audio public-API stress
task; never enable either test mode in a production image.

`sdkconfig.phase16-auto-hil` is a reproducibility reference for an isolated
test configuration. With an existing local `sdkconfig`, explicitly enable the
symbol through menuconfig or a scoped local configuration edit; defaults do
not override an already-set sdkconfig value.

| Cases | Automated evidence | Limitation |
|---|---|---|
| T16-01 | Actual host build; runtime marker is supplemental only | Runtime marker alone is not build evidence |
| T16-02 | GUI/audio/arbiters quiescent and voice/Xiaozhi READY | Bounded 30-second connect window |
| T16-03, T16-04 | Explicit `SKIP` | Still require a real PTT, remote Xiaozhi TTS, and speaker evidence |
| T16-05 | Synthetic Xiaozhi playback plus notification queue/no preempt | Requires the WAV asset |
| T16-06 | Synthetic Xiaozhi playback plus alarm cooperative preempt/promotion | Requires the WAV asset |
| T16-07 | Equal-priority PREEMPT rejected without preemption | Requires the WAV asset |
| T16-08 | Current plus pending plus deterministic third reject | Requires the WAV asset |
| T16-09 | Synthetic capture contention: equal reject and higher known client preempts | No GPIO/PTT; microphone hardware is still exercised |
| T16-10 | Back-to-back requests measured under 20 ms; winner ACTIVE, loser either sees busy/retries or remains retained while the opposite resource is active, then becomes ACTIVE | Does not prove every arbitrary multi-task interleaving |
| T16-11 | Pending-before-start, pending, and active cancellation | Requires the WAV asset |
| T16-12 | Sensor/cloud/SD/UI/voice/audio snapshots and bounded resource trend | Not a long-duration leak test |

If the SD VFS is unavailable, WAV-dependent cases are SKIP. If the SD is
mounted but the configured WAV is absent or malformed, the affected case is
FAIL because the playback prerequisite itself failed; inspect the audio-manager
error evidence before classifying it as an arbitration-policy defect.

### 7. Manual-action protocol

Ask the user only for actions that genuinely require physical interaction, for example:

```text
MANUAL ACTION: press and hold PTT, speak the requested phrase, then release.
MANUAL ACTION: interrupt network connectivity now.
MANUAL ACTION: confirm speaker output/GUI behavior.
```

Do not ask the user to manually infer arbiter counters or internal ownership when serial instrumentation can prove it.

### 8. Required Phase-16 stimuli

Test-only code may provide deterministic helpers for:

- notification WAV request while Xiaozhi playback is active;
- critical alarm WAV request while Xiaozhi playback is active;
- equal-priority PREEMPT request;
- current + pending + third-request queue pressure;
- second capture request while Xiaozhi is recording;
- near-simultaneous CAPTURE/PLAYBACK submissions;
- cancel-before-start/current/pending cases.

All test requests require explicit unique request IDs. Do not use an unbounded request generator.

### 9. Evidence to print/capture

For arbitration cases capture at least:

```text
request_id
client
resource
priority
busy_policy
interruptible
arbiter state
current request
pending request
accepted/rejected/queued/preempted/completed/failed counters
audio_manager state
```

For preemption, evidence must show the order:

```text
XIAOZHI PLAYBACK active
-> ALARM priority 100 accepted as PREEMPT
-> cooperative audio_manager_stop_playback
-> manager IDLE
-> ALARM promoted
-> manager PLAYBACK
```

For notification while Xiaozhi speaks:

```text
XIAOZHI priority 70 active
-> NOTIFICATION priority 50
-> no preempt
-> queued if pending free, otherwise deterministic reject
```

### 10. PASS/FAIL rules

Never infer PASS from source inspection.

```text
IMPLEMENTED    source/test harness exists
BUILD VERIFIED clean compile/link evidence exists
HIL PASS       real target evidence satisfies acceptance
```

A case is PASS only with runtime evidence required by the plan. If hardware/service/manual prerequisite is unavailable, mark SKIP with reason rather than PASS.

### 11. Production defect policy

If HIL exposes a production defect:

```text
identify owning production code
-> document failure/evidence on this test branch
-> fix production defect first on phase/16-audio-arbitration (or earlier owning phase if applicable)
-> propagate the fix forward/back into this test branch
-> rebuild + rerun affected cases
```

Do not hide production defects with test-only workarounds.

### 12. Final Codex report

Return a compact matrix:

```text
T16-01 BUILD                    PASS/FAIL/SKIP
T16-02 BOOT                     PASS/FAIL/SKIP
T16-03 XIAOZHI CAPTURE          PASS/FAIL/SKIP
T16-04 XIAOZHI PLAYBACK         PASS/FAIL/SKIP
T16-05 NOTIFICATION QUEUE       PASS/FAIL/SKIP
T16-06 ALARM PREEMPT            PASS/FAIL/SKIP
T16-07 EQUAL PRIORITY           PASS/FAIL/SKIP
T16-08 QUEUE PRESSURE           PASS/FAIL/SKIP
T16-09 CAPTURE CONTENTION       PASS/FAIL/SKIP
T16-10 CAPTURE/PLAYBACK RACE    PASS/FAIL/SKIP
T16-11 CANCELLATION             PASS/FAIL/SKIP
T16-12 FULL GATEWAY             PASS/FAIL/SKIP
```

Then report:

- first failing test/root-cause hypothesis;
- production files changed, if any;
- test-only files changed;
- build/flash command used;
- relevant serial-log evidence;
- whether Phase 16 can be marked HIL ACCEPTED.

## Expected high-level successful log story

```text
PH16_TEST T16_01 PASS evidence=build_linked_auto_harness
PH16_TEST T16_02 PASS
PH16_TEST T16_03 SKIP reason=requires_real_xiaozhi_ptt_and_microphone_turn
PH16_TEST T16_04 SKIP reason=requires_real_xiaozhi_network_tts_and_speaker_evidence
PH16_TEST T16_05 PASS
PH16_TEST T16_06 PASS
PH16_TEST T16_07 PASS
PH16_TEST T16_08 PASS
PH16_TEST T16_09 PASS
PH16_TEST T16_10 winner=<CAPTURE|PLAYBACK> loser=<PLAYBACK|CAPTURE> wait=<WAIT_STATUS_BUSY|RETRY_AFTER_INVALID_STATE>
PH16_TEST T16_10 PASS
PH16_TEST T16_11 PASS
PH16_TEST T16_12 PASS
PH16_TEST SUMMARY pass=<n> fail=0 skip=<n>
```

These are expected case-completion markers, not a literal serial transcript.
Actual sanitized target results are recorded in
`AI_Stored_Data/PHASE16_HIL_EVIDENCE.md`.

## Current evidence

```text
Production Phase-16 SW        COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED
Dedicated Phase-16 test branch SYNCHRONIZED through 7faeed8
Codex HIL command/runbook      EXECUTED
Test-branch idf.py build       PASS (test-enabled 0x221b30; final default-off 0x21e7b0; 47% app free)
Flash/run                      PASS (automatic target summary: pass=10 fail=0 skip=2)
Real PTT/speaker cases         PASS (prior operator-confirmed Phase-16 HIL)
Phase-16 HIL acceptance        ACCEPTED (bounded matrix)
Evidence record                AI_Stored_Data/PHASE16_HIL_EVIDENCE.md
```
