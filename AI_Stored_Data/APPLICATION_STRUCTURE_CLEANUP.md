# Application Structure Cleanup

Updated: 2026-09-13
Integration branch: `main_including_Firebase_security`
Integrated source commit: `0a8c83f7776d8259f22208a66f7fc4bd52156aff` (`Cleanup code structure`)

## Status

**INTEGRATED / BEHAVIOR-PRESERVING SOURCE-ORGANIZATION CLEANUP**

The application-structure cleanup is no longer an unmerged refactor branch. Its
current structure is present directly on `main_including_Firebase_security`.

## Current production structure

```text
main/
  main.c                         thin ESP-IDF entrypoint

components/application/
  smart_room_app/                product composition/startup/callback routing
  smart_room_mcp_adapter/        Smart Room domain <-> MCP provider adaptation
  app_network_coordinator/       network/provisioning application policy
  app_reset_coordinator/         reset transaction orchestration
  voice_assistant/               voice-session orchestration
  xiaozhi_foundation/            direct managed esp_xiaozhi/MCP boundary
```

`main/main.c` only calls `smart_room_app_start()`.

## Ownership after cleanup

- `smart_room_app` owns startup ordering, product policy values, and copied
  cross-component callback routing. It does not own manager/driver internals.
- `smart_room_mcp_adapter` owns Smart Room provider adaptation through public
  service APIs. It never owns the MCP engine/session, GPIO/RMT, I2S, LVGL, or
  domain-manager resources.
- `xiaozhi_foundation` remains the sole direct managed `esp_xiaozhi` and MCP
  engine/session boundary.
- Domain managers retain their established ownership (`light_manager`,
  `audio_manager`, `sensor_manager`, `cloud_manager`, and others).
- Dependency direction remains `application -> service -> driver/framework`.

## Retired cleanup/test artifacts

The cleanup removed production-tree test/legacy artifacts that were no longer
part of the normal product configuration, including the former Phase-16 target
HIL coordinator, legacy direct-I2S `audio_test`, and audio public-API stress
configuration/support.

Historical HIL evidence remains historical evidence; removal of an old harness
does not erase its accepted results.

## Validation/evidence discipline

The cleanup handoff recorded a normal ESP-IDF build PASS after the structural
moves. No new target HIL was performed specifically for the structure cleanup.
Previously accepted product/phase HIL remains separate evidence and must not be
misrepresented as a post-cleanup hardware run.

## Scope guard

This cleanup did not intentionally change MCP tool contracts, light behavior,
audio arbitration, transport/protocol behavior, credentials, hardware mapping,
or roadmap scope. In particular, Phase 18.2 remains **NOT STARTED**.
