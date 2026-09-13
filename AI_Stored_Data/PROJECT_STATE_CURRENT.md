# Smart Room Cloud Gateway — Current Project State

Updated: 2026-09-13
Active integration branch: `main_including_Firebase_security`
Current production/source baseline before documentation-only synchronization: `0a8c83f7776d8259f22208a66f7fc4bd52156aff` (`Cleanup code structure`)

> This file is a current-state companion while older handoff records are being
> reconciled. Current source and `AGENTS.md` remain higher authority.

## Current architecture

```text
main/main.c
    -> smart_room_app
        -> product startup/order/policy and copied callback routing
        -> smart_room_mcp_adapter
            -> project-owned provider adaptation
            -> xiaozhi_foundation MCP/provider boundary
                -> managed esp_xiaozhi / MCP session
```

Ownership remains:

- `main`: thin ESP-IDF entrypoint only.
- `smart_room_app`: product composition, startup policy, application-level
  configuration, and copied callback routing.
- `smart_room_mcp_adapter`: Smart Room domain/provider adaptation using public
  service APIs only.
- `xiaozhi_foundation`: sole direct managed `esp_xiaozhi` / MCP engine and
  session boundary.
- `audio_manager`: sole microphone/speaker I2S, DMA, PCM, recording and
  playback resource owner.
- `light_manager`: product light state/effect owner; NeoPixel remains below it.
- `sensor_manager`, `cloud_manager`, `wifi_manager`, `config_manager`,
  `sd_card_manager`, `app_gui`, and other managers retain their established
  ownership.
- dependency direction remains `application -> service -> driver/framework`.

The application-structure cleanup is integrated into
`main_including_Firebase_security`; it is no longer an unmerged refactor. See
`AI_Stored_Data/APPLICATION_STRUCTURE_CLEANUP.md`.

## Current roadmap state

```text
Sprint 12   COMPLETE / HIL PASS
Sprint 13   COMPLETE / HIL PASS
Sprint 14   SOFTWARE COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Sprint 15   COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Sprint 16   COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1  COMPLETE BASELINE / streaming HIL accepted / endurance pending
Sprint 17   COMPLETE / read-only MCP voice HIL accepted
Sprint 18   MCP CONTROLLED ACTIONS / IN PROGRESS
Phase 18.1  COMPLETE / build PASS / target HIL accepted by Hải on 2026-09-13
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

Phase 18.1 remains closed after the source-structure cleanup because the cleanup
was intended to preserve behavior. No new target HIL was run specifically after
the structural move; prior Phase-18.1 HIL acceptance and the cleanup build
record are separate evidence.

## Approved post-Sprint-18 roadmap

The 2026-09-13 approved roadmap preserves all Sprint 0-18 history and all current
Phase 18.x numbering/scope. Only future work after Sprint 18 is allocated as
follows:

```text
19  Local Web Control V1: SD Card File Manager
20  Local Web Control V2: Playback + Volume
21  Local Web Control V3: Lights
22  Local Web Control V4: Dashboard + System Status
23  Local Web Control V5: Scenes + Logs + Diagnostics
24  Wake Word + Advanced Voice UX
```

The web roadmap is **SD-card-first**. The local Web UI is for an already-networked
device; it must not add Wi-Fi configuration or Wi-Fi lifecycle control. Web and
LCD remain sibling frontends over existing manager/service ownership boundaries.
Advanced OTA/factory-management flows remain outside the current scope.

The former Sprint 19 Wake Word plan is deferred to Sprint 24 without changing
its required sequence: feasibility/resource audit -> continuous local capture +
WakeNet/VAD -> advanced conversation -> endurance/HIL closure.

Durable web scope and anti-drift rules are recorded in
`AI_Stored_Data/LOCAL_WEB_DASHBOARD_PLAN.md`.

## Phase 18.1 accepted contract

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

Accepted semantics:

- `power=off` cannot be combined with color, brightness, or effect.
- color-only and brightness-only requests preserve logical power.
- effect without explicit power activates the light.
- effect without explicit color uses white when preserved RGB is black.
- invalid requests produce no `light_manager` side effect.
- fixed effect timing remains: blink 500/500 ms, breath 2000 ms, pulse 1200 ms,
  rainbow 10 ms step.

Current provider path after structure cleanup:

```text
Xiaozhi/MCP tool
-> xiaozhi_foundation
-> smart_room_mcp_adapter provider
-> owning manager/service
-> bounded copied result
```

## Voice / TLS / streaming facts

Current configuration/source behavior retained from the accepted voice path:

- dynamic mbedTLS buffers use PSRAM;
- outbound TLS record size is 1 KiB;
- PTT checks both total and largest-contiguous PSRAM and requires 20 KiB
  headroom before starting transport/capture;
- user-confirmed repeated PTT/TLS smoke passed during Phase-18.1 acceptance;
- PCM streaming uses a 7.68-second bounded PSRAM ingress ring;
- normal prefill target is 0.96 seconds;
- the 5-second prefill wait starts after the first PCM packet, not at
  `TTS_START`;
- delayed-first-PCM regression remains a separate deferred regression unless a
  newer target run records it explicitly.

## Structure cleanup status

Integrated source commit `0a8c83f...`:

- reduced `main/main.c` to the application entrypoint;
- introduced `smart_room_app` for product composition;
- introduced `smart_room_mcp_adapter` for MCP-domain provider adaptation;
- retired the old Phase-16 target-HIL coordinator and legacy direct-I2S
  `audio_test` production-tree files;
- retired audio public-API stress configuration/support from normal product
  configuration;
- did not intentionally add Phase-18.2 or change product behavior.

The cleanup handoff recorded a normal ESP-IDF build PASS. No target HIL was run
specifically for the cleanup.

## Current deferred work

These do not reopen Phase 18.1:

1. delayed-first-PCM / streaming regression on the post-change voice path;
2. Phase-16/16.1 endurance and long-duration resource trend checks;
3. long-duration Firebase/cloud + Xiaozhi simultaneous-traffic regression;
4. relevant UI regression when a future defect touches that path;
5. bounded post-structure-cleanup target smoke if desired before a release
   checkpoint.

Do not start Phase 18.2 or any Sprint 19-24 implementation automatically. Start
new implementation only when Hải explicitly requests the relevant phase/sprint.

## Security invariants

Never store real Wi-Fi credentials, Firebase passwords/API secrets, PoP values,
private keys, service-account JSON, access/refresh tokens, activation secrets,
or private transport payloads in tracked source or `AI_Stored_Data/`.
