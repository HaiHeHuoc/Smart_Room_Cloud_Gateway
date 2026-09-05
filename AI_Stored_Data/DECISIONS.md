# Smart Room Cloud Gateway — Durable Decisions

This file records compact decisions that future AI sessions should preserve unless Hải explicitly changes them or newer repository evidence supersedes them.

## DECISION — AI handoff directory

Date: 2026-08-25

`AI_Stored_Data/` is the shared repository-local synchronization area for important AI handoff context across ChatGPT conversations, Codex, and other AI-assisted workflows.

AI assistants are authorized by Hải to create, overwrite, reorganize, and update content inside this directory without treating those metadata edits as production architecture changes.

Constraints:

- the directory may be deleted by Hải at any time;
- firmware/build/runtime code must not depend on it;
- canonical source/docs remain higher authority;
- never store credentials, tokens, activation secrets, private payloads, or other sensitive data here.

## DECISION — Repository AI operating guide

Use `AGENTS.md` as the repository-specific operating guide before implementation/review work.

Important consequences:

- inspect before editing;
- preserve roadmap and completed phase history;
- stay inside requested phase scope;
- do not over-engineer;
- separate confirmed evidence from assumptions;
- do not claim build or hardware results that were not actually observed.

## DECISION — Xiaozhi ownership boundary

Only the project Xiaozhi boundary should directly depend on `esp_xiaozhi`; external Xiaozhi handles/types/pointers/transport objects must not leak into unrelated public component APIs.

Xiaozhi/network/audio callbacks must not directly own LVGL, Wi-Fi lifecycle, provisioning, project NVS reset, reboot, OTA, or arbitrary hardware actions.

## DECISION — Phase 12 transport

For Xiaozhi Phase 12, the selected project transport is **WebSocket only**.

If WebSocket is unavailable, report unavailable. Do not add MQTT+UDP fallback.

The upstream component may persist server-returned MQTT data internally, but the project does not select or expose MQTT as its Xiaozhi transport.

## DECISION — No typed-text workaround

Pinned/resolved `esp_xiaozhi` 0.1.2 does not expose an arbitrary typed-text TX API for the Phase 12 validation need.

Do not bypass that limitation with private/raw protocol calls or undocumented transport messages. P2-F must use a lawful audio fixture and public supported APIs.

## DECISION — Temporary validation isolation

Phase 12 validation infrastructure is not production voice-assistant behavior.

The master Kconfig validation gate remains default OFF. Normal Gateway behavior with the gate disabled must remain free of automatic Xiaozhi validation worker, validation screen route, observer registration, and transport-validation requests.

Production `voice_assistant` ownership/state-machine work belongs to Sprint 13 unless Hải explicitly changes the roadmap.

## DECISION — Roadmap continuity

Do not skip, replace, or silently close existing phases. New work must respect the established roadmap and acceptance state. Cleanup work must not silently become a new feature phase.
