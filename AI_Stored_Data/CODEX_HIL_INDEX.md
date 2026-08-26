# Codex HIL Index

Updated: 2026-08-26
Development branch: `phase/14-ptt-voice-mvp`

The operator plans to connect target hardware to the local development machine and use Codex as the HIL test operator/analyst.

Phase 12 and Phase 13 target acceptance passed on 2026-08-25. Phase 14
golden-path target acceptance passed on 2026-08-26 after three audible PTT
turns on GPIO38. The next hardware acceptance is:

1. Phase 15 — checkout `test/phase15-voice-ui-hil` after the Phase-14 Git
   checkpoint is propagated into `phase/15-voice-assistant-ui`.
2. On that test branch, read `AI_Stored_Data/PHASE15_HIL_TEST_BRANCH.md` and
   `AI_Stored_Data/PHASE15_HIL_TEST_PLAN.md`.
3. Use activation label `RUN PHASE 15 HIL` only after the required display,
   PTT/audio and network hardware is ready.

Codex may build, flash, monitor serial, analyze evidence, make minimal evidence-backed fixes, reflash/retest, and update HIL documentation when the local environment exposes ESP-IDF and the ESP32-S3 serial device.

Codex must ask the operator only for physical/environment actions it cannot perform itself, such as SD-card preparation, AP/Internet toggling, button presses, power cycling, or visual/audio observations.

Never treat build success as HIL PASS. Missing manual evidence is `SKIP` or
`PENDING`, not PASS. Preserve the complete first failing log before modifying
code. Phase-14 fault-injection cases remain explicitly deferred even though
its documented golden path passed.

For the general next-work routing rule, also read `AI_Stored_Data/NEXT_WORK_AND_HIL_BACKLOG.md`.
