# Smart Room Cloud Gateway — AI Project State

Updated: 2026-09-13
Active integration branch: `main_including_Firebase_security`
Production/source baseline before this documentation-only synchronization: `0a8c83f7776d8259f22208a66f7fc4bd52156aff` (`Cleanup code structure`)

## Working authority

Use this priority when resuming work:

1. Current source/build configuration on the active branch.
2. `AGENTS.md` and current canonical repository documentation.
3. Explicit recent build/HIL/manual evidence from Hải.
4. `AI_Stored_Data/` handoff notes.
5. Conversation memory/assumptions.

`AI_Stored_Data/` is cross-session support metadata only. Production firmware,
CMake, Kconfig, tests, and runtime code must never depend on it.

## Application structure cleanup — integrated

The application-structure cleanup is now part of
`main_including_Firebase_security`; it is no longer an unmerged refactor.
Integrated source commit:

```text
0a8c83f7776d8259f22208a66f7fc4bd52156aff
Cleanup code structure
```

Current production structure:

```text
main/main.c
    -> smart_room_app
        -> product startup/order/policy and copied callback routing
        -> smart_room_mcp_adapter
            -> Smart Room provider adaptation
            -> xiaozhi_foundation
                -> managed esp_xiaozhi / MCP engine and session
```

Key boundaries:

- `main` is a thin ESP-IDF entrypoint only.
- `smart_room_app` owns product composition, startup ordering, application policy
  values, and copied cross-component callback routing.
- `smart_room_mcp_adapter` owns Smart Room provider adaptation using only public
  service APIs.
- `xiaozhi_foundation` remains the sole direct managed `esp_xiaozhi`/MCP
  engine/session boundary.
- managers/drivers retain their existing domain ownership.
- dependency direction remains `application -> service -> driver/framework`.

The cleanup also retired the former Phase-16 target-HIL coordinator, legacy
direct-I2S `audio_test` production-tree files, and the audio public-API stress
configuration/support that was no longer part of the normal product profile.

The cleanup handoff recorded a normal ESP-IDF build PASS after the structural
moves. No target HIL was run specifically for this structure cleanup; prior
phase HIL and the cleanup build are separate evidence.

See `AI_Stored_Data/APPLICATION_STRUCTURE_CLEANUP.md`.

## Current high-level phase state

```text
Sprint 12   COMPLETE / HIL PASS
Sprint 13   COMPLETE / HIL PASS
Sprint 14   SOFTWARE COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Sprint 15   COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Sprint 16   COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1  COMPLETE BASELINE / streaming HIL accepted / endurance pending
Sprint 17   COMPLETE / read-only MCP voice HIL accepted
Sprint 18   MCP CONTROLLED ACTIONS IN PROGRESS
Phase 18.1  COMPLETE / BUILD PASS / target HIL accepted by Hải on 2026-09-13
Phase 18.2  NOT STARTED
Phase 18.3  NOT STARTED
Phase 18.4  NOT STARTED
Sprint 19   Local Web Control V1: SD Card File Manager / PLANNED / NOT STARTED
Sprint 20   Local Web Control V2: Playback + Volume / PLANNED / NOT STARTED
Sprint 21   Local Web Control V3: Lights / PLANNED / NOT STARTED
Sprint 22   Local Web Control V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23   Local Web Control V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24   Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

Phase 18.1 remains closed after the behavior-preserving source-structure cleanup.
A post-cleanup target HIL run has not been recorded; do not relabel the earlier
accepted HIL as a post-cleanup hardware run.

## Approved roadmap after Sprint 18

The 2026-09-13 roadmap decision leaves Sprint 0-18 history and the existing
Sprint-18 Phase 18.x scope unchanged. Future allocation is:

```text
Sprint 19  Local Web Control V1: SD Card File Manager
Sprint 20  Local Web Control V2: Playback + Volume
Sprint 21  Local Web Control V3: Lights
Sprint 22  Local Web Control V4: Dashboard + System Status
Sprint 23  Local Web Control V5: Scenes + Logs + Diagnostics
Sprint 24  Wake Word + Advanced Voice UX
```

The Local Web roadmap is SD-card-first. It runs as a frontend over existing
manager/service boundaries and is not a Wi-Fi provisioning/configuration/control
surface. The LCD remains a sibling frontend rather than a subordinate web
implementation. Advanced OTA and factory-management features remain outside the
current scope.

The former Sprint 19 Wake Word plan is deferred to Sprint 24 while preserving
its sequence: feasibility/resource audit -> continuous local capture plus
WakeNet/VAD -> advanced conversation -> endurance/HIL closure.

Use `AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md` as the durable detailed web plan.

## Phase 18.1 current contract

Production MCP tools:

```text
light.set_state
light.get_state
light.get_capabilities
```

Allowed controlled fields:

```text
power               on | off
color               red | green | blue | white | yellow | cyan |
                    magenta | pink | purple | orange
brightness_percent  integer 0..100
effect              solid | blink | breath | pulse | rainbow
```

Current semantics:

- `power=off` cannot be combined with color, brightness, or effect.
- color-only/brightness-only preserve logical power.
- effect without explicit power activates the light.
- effect without explicit color uses white when the preserved RGB is black.
- validation failures produce no `light_manager` side effect.
- current fixed effect timing: blink 500/500 ms, breath 2000 ms, pulse 1200 ms,
  rainbow 10 ms step.

Current ownership path after the structure cleanup:

```text
User intent / Xiaozhi backend
-> xiaozhi_foundation MCP tool
-> smart_room_mcp_adapter provider
-> light_manager
-> NeoPixel / hardware
-> bounded copied result
```

MCP never owns GPIO, RMT, NeoPixel, LVGL, I2S, or unrelated domain resources.

## Voice / TLS / streaming source facts

Current source/configuration retains:

```text
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=1024
CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set
```

The PTT path checks both total and largest-contiguous PSRAM before starting a
turn and requires at least 20 KiB headroom. Hải confirmed repeated PTT/TLS smoke
PASS during the Phase-18.1 acceptance sequence.

Streaming downlink currently uses:

- a 7.68-second bounded PSRAM ingress ring;
- 0.96-second normal prefill;
- a 5-second prefill wait that starts only after the first PCM packet;
- separate bounded post-start starvation recovery.

The delayed-first-PCM regression remains separate deferred validation unless a
newer target run explicitly records it.

## Established ownership boundaries

- `main`: thin ESP-IDF entrypoint.
- `smart_room_app`: product composition/startup/application callback routing.
- `smart_room_mcp_adapter`: project-owned MCP provider adaptation.
- `app_network_coordinator`: network/provisioning orchestration.
- `wifi_manager`: Wi-Fi Station lifecycle/reconnect.
- `provisioning_manager`: temporary BLE provisioning transport.
- `config_manager`: durable application configuration.
- `audio_manager`: sole microphone/speaker I2S, DMA, PCM, recording/playback
  resource owner.
- `xiaozhi_foundation`: sole direct managed Xiaozhi/MCP engine/session boundary.
- `voice_assistant`: product voice-session/recovery orchestration.
- `app_gui`: product screens/models/UI queues.
- `ui_manager_lvgl`: LVGL runtime/synchronization owner.
- `sensor_manager`: sensor sampling/staleness owner.
- `cloud_manager`: telemetry/retry owner.
- `sd_card_manager`: SD/VFS lifecycle/lease owner.
- `light_manager`: product light state/effect owner.
- `board_config.h`: physical board mapping source of truth.

GPIO ownership:

```text
GPIO9   factory-reset input, active high
GPIO38  PTT input, active high, internal pull-down plus recommended external pull-down
GPIO48  NeoPixel; never use as PTT
```

## Portability/structure rules

The portability-hardening architecture is integrated and frozen. Preserve:

- `application -> service -> driver/framework`;
- domain folders as organizational containers;
- parent-owned private `modules/<name>/` for tightly coupled subsystems;
- no cross-component inclusion of another component's private-module headers;
- product/application components remain product-specific unless real reuse or
  lifecycle evidence justifies generalization.

The later application-structure cleanup applies those rules to product
composition; it is not a new portability wave.

## Current deferred validation / technical debt

These items do not reopen Phase 18.1:

1. delayed-first-PCM / streaming regression on the current voice path;
2. Phase-16/16.1 endurance and long-duration resource trend checks;
3. long-duration Firebase/cloud + Xiaozhi simultaneous traffic;
4. relevant Phase-15 UI/text regression only when a future defect touches that
   path;
5. bounded post-structure-cleanup target smoke if desired before a release
   checkpoint.

## Recommended next action

Do not start Phase 18.2 automatically. Phase 18.2 starts only when Hải
explicitly requests it. Sprints 19-24 are approved roadmap entries but remain
PLANNED / NOT STARTED until explicitly started. Documentation consistency work
may continue without changing firmware scope.

## Security invariants

Never store real Wi-Fi credentials, Firebase passwords/API secrets, PoP values,
private keys, service-account JSON, access/refresh tokens, activation secrets,
or private transport payloads in tracked source or `AI_Stored_Data/`.
