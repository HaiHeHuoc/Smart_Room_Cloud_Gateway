# Codex HIL Index

Updated: 2026-08-25
Development branch: `phase/13-voice-assistant`

The operator plans to connect target hardware to the local development machine and use Codex as the HIL test operator/analyst.

When hardware is available, run deferred acceptance in this order unless a specific regression requires otherwise:

1. Phase 12 — checkout `test/xiaozhi-p2f-known-audio-e2e` and read `AI_Stored_Data/CODEX_HIL_RUNBOOK.md` plus the Phase-12 HIL documents.
2. Phase 13 — checkout `test/phase13-voice-assistant-hil` and read `AI_Stored_Data/CODEX_HIL_RUNBOOK.md` plus the Phase-13 HIL documents.

Codex may build, flash, monitor serial, analyze evidence, make minimal evidence-backed fixes, reflash/retest, and update HIL documentation when the local environment exposes ESP-IDF and the ESP32-S3 serial device.

Codex must ask the operator only for physical/environment actions it cannot perform itself, such as SD-card preparation, AP/Internet toggling, button presses, power cycling, or visual/audio observations.

Never treat build success as HIL PASS. Missing manual evidence is `SKIP` or `PENDING`, not PASS. Preserve the complete first failing log before modifying code.

For the general next-work routing rule, also read `AI_Stored_Data/NEXT_WORK_AND_HIL_BACKLOG.md`.
