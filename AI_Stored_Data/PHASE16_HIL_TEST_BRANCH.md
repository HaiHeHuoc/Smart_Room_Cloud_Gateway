# Phase 16 Audio Arbitration HIL Test Branch

Updated: 2026-09-02
Test branch: `test/phase16-audio-arbitration-hil`
Base/production branch: `phase/16-audio-arbitration`
Activation command: `RUN PHASE 16 HIL`
Status: **TEST BRANCH SYNCHRONIZED / BUILD VERIFIED / HARDWARE EXECUTION PENDING**

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

Minimum evidence:

```text
PH16_TEST BUILD_BEGIN
PH16_TEST BUILD_PASS
```

If build fails:

```text
PH16_TEST BUILD_FAIL reason=<short reason>
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
PH16_TEST BOOT_PASS
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
PH16_TEST BUILD_PASS
PH16_TEST T16_02 PASS
PH16_TEST XIAOZHI_CAPTURE owner=XIAOZHI priority=70 state=ACTIVE
PH16_TEST XIAOZHI_PLAYBACK owner=XIAOZHI priority=70 state=ACTIVE
PH16_TEST NOTIFICATION decision=WAIT_OR_QUEUE preempt=0
PH16_TEST ALARM decision=PREEMPT priority=100
PH16_TEST EQUAL_PRIORITY preempt=0
PH16_TEST QUEUE third_request=REJECT
PH16_TEST RACE duplicate_hardware_owner=0
PH16_TEST T16_12 PASS
PH16_TEST SUMMARY pass=<n> fail=0 skip=<n>
```

These are semantic expected markers for the test harness; do not claim they have already been observed on hardware.

## Current evidence

```text
Production Phase-16 SW        COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED
Dedicated Phase-16 test branch SYNCHRONIZED through 7faeed8
Codex HIL command/runbook      READY
Test-branch idf.py build       PASS (ESP-IDF 6.0.1; 0x21e7b0; 47% app free)
Flash/run                      NOT YET CLAIMED
Phase-16 HIL acceptance        PENDING
```
