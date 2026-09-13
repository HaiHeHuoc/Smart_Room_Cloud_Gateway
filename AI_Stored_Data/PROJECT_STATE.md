# Smart Room Cloud Gateway — AI Project State

Updated: 2026-09-13
Integration base branch: `main_including_Firebase_security`
Observed production/source HEAD before this AI metadata synchronization: `15cd0f06d25142a6ed7672bc99dfd4ec396184b0`

## Working authority

Use this priority when resuming work:

1. Current source/build configuration on the active branch.
2. `AGENTS.md` and current canonical repository documentation.
3. Explicit recent build/HIL/manual evidence from Hải.
4. `AI_Stored_Data/` handoff notes.
5. Conversation memory/assumptions.

`AGENTS.md` is the repository/session operating authority. Preserve phase history,
ownership boundaries, evidence discipline, and the rule that build/HIL success
must never be claimed without evidence.

`AI_Stored_Data/` is cross-session support metadata only. Production firmware,
CMake, Kconfig, tests, and runtime code must never depend on it.

This snapshot is based on the remote GitHub branch. A local worktree with
uncommitted or unpushed changes cannot be observed through this handoff and must
be inspected separately before implementation work.

## Unmerged application-structure cleanup

The local branch `refactor/application-structure-cleanup` is a focused,
behavior-preserving source-organization change based on
`main_including_Firebase_security`. It is not an integration baseline until a
separate reviewed merge is authorized.

- `main/main.c` is now a thin ESP-IDF entrypoint that calls `smart_room_app`.
- `smart_room_app` owns product startup order, application policy values, and
  copied cross-component callback routing; managers and drivers keep their
  existing ownership.
- `smart_room_mcp_adapter` consolidates the existing sensor, cloud-sync,
  system-status, light-state, and light-set-state provider bridges. The
  `xiaozhi_foundation` MCP engine/session lifecycle remains unchanged.
- The Phase-16 target-HIL coordinator was retired during pre-base cleanup.
  The retired direct-I2S `audio_test` source remains outside the production
  application under `test_apps/audio_legacy_test`.
- This refactor adds no MCP tool, light/audio behavior, protocol, credential,
  board mapping, or Phase-18.2 work. Phase 18.2 remains **NOT STARTED**.

The normal ESP-IDF build passed on this branch after the structural moves.
No target HIL was run as part of the refactor; prior user-confirmed HIL evidence
remains separate from this source-organization build result.

## Current high-level phase state

```text
Sprint 12   COMPLETE / HIL PASS
Sprint 13   COMPLETE / HIL PASS
Sprint 14   SOFTWARE COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Sprint 15   COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Sprint 16   COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1  STREAMING DOWNLINK IMPLEMENTED / BUILD VERIFIED / automated HIL PASS / audible recovery accepted / endurance pending
Sprint 17   MCP READ-ONLY COMPLETE / BUILD VERIFIED / voice HIL accepted by user
Sprint 18   MCP CONTROLLED ACTIONS IN PROGRESS
Phase 18.1  COMPLETE / BUILD PASS / target HIL accepted by user (2026-09-13)
Phase 18.2  NOT STARTED
Phase 18.3  NOT STARTED
Phase 18.4  NOT STARTED
Sprint 19   NOT STARTED
```

Detailed Phase-18 record: `AI_Stored_Data/PHASE18_MCP_CONTROLLED_ACTIONS.md`.

## Current branch/source anchor

Relevant recent source history:

```text
f00e106150ddf2a48034a1ed9b6c6520aff20fc5
  feat(light): add bounded effects and capabilities [18.1]
  Explicit evidence: full ESP-IDF build PASS; target HIL pending.

e0255881ad61a5bea4c96b49c866f20a0f8b3355
  fix(tls): reduce PTT-time WebSocket allocation
  Explicit evidence: clean ESP-IDF build PASS; target boot/repeated PTT pending.

15cd0f06d25142a6ed7672bc99dfd4ec396184b0
  work for MCP LED control
  Changes Phase-18 light semantics plus voice/audio/TLS behavior.
  Current-source build PASS; Phase-18.1 target HIL accepted by user (2026-09-13).
```

The source checkpoint `15cd0f06...` was rebuilt after its source changes.
Phase-18.1 target HIL was accepted by the user on 2026-09-13. Later metadata
commits do not modify production source.

AI-only synchronization commits may advance the branch HEAD beyond
`15cd0f06...`; treat those as metadata/documentation history, not as newer
production validation baselines.

## Recent source changes after the last Phase-18.1 build checkpoint

Comparison `f00e106... -> 15cd0f06...` contains two commits and modifies:

```text
components/application/voice_assistant/modules/uplink/src/voice_assistant_uplink.c
components/application/xiaozhi_foundation/docs/README.md
components/application/xiaozhi_foundation/modules/mcp_light_set_state/src/xiaozhi_mcp_light_set_state.c
components/audio/audio_manager/audio_manager.c
components/output/light_manager/README.md
components/output/light_manager/light_manager.c
components/system/common/include/app_common.h
main/xiaozhi_light_set_state_composition.c
sdkconfig.defaults
```

### TLS / PTT memory policy at current source HEAD

Current `sdkconfig.defaults` uses dynamic mbedTLS buffers in PSRAM:

```text
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=1024
CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set
CONFIG_MBEDTLS_HARDWARE_AES is not set
```

The code now checks both total and largest contiguous PSRAM before starting a
PTT turn and requires at least 20 KiB. A low-memory turn is rejected before
transport/capture rather than waiting for a WebSocket write failure after I2S
capture starts.

This intentionally differs from older notes that treated library-owned TLS
records as Internal-RAM-only. Current source is authoritative. The source
comment also notes that a production product requiring physical-memory
confidentiality must pair TLS-in-PSRAM with the appropriate ESP32-S3 flash/
external-memory protection strategy; do not infer that such protection is
currently enabled without configuration evidence.

### Streaming-downlink prefill behavior at current source HEAD

The PCM stream still uses a 7.68-second bounded PSRAM ingress ring, but the
normal prefill target is currently 0.96 seconds, not the older 1.44-second value
recorded in historical notes.

The 5-second bounded prefill timeout now begins only after the first PCM packet
has actually arrived. `TTS_START` may precede the first audio packet while the
server performs tool work or synthesis; that pre-audio delay is no longer
charged against the PCM prefill allowance. Post-start starvation recovery
remains separately bounded.

Any future documentation mentioning 1.44-second prefill should be checked
against current source before reuse.

## Phase 18.1 current behavior

Implemented MCP surface:

```text
light.set_state
light.get_state
light.get_capabilities
```

Allowed controlled fields:

```text
power: on/off
color: red, green, blue, white, yellow, cyan, magenta, pink, purple, orange
brightness_percent: integer 0..100
effect: solid, blink, breath, pulse, rainbow
```

Important current semantics:

- `power=off` cannot be combined with color, brightness, or effect.
- Color-only/brightness-only preserve current logical power.
- Effect without explicit power activates the light.
- Effect without explicit color uses white if the preserved RGB is black.
- `light_manager` remains the hardware owner; MCP never drives GPIO/RMT/
  NeoPixel directly.
- Current fixed effect timings are blink 500/500 ms, breath 2000 ms, pulse
  1200 ms, rainbow 10 ms step.

The earlier 300 ms pulse value and the older "effect-only preserves OFF" rule
are stale and must not be reused.

## Established ownership boundaries

Preserve these unless an explicitly approved phase changes them:

- `main`: thin ESP-IDF entrypoint into product composition.
- `smart_room_app`: product composition, startup policy, and copied callback
  routing.
- `smart_room_mcp_adapter`: application-owned MCP provider adaptation through
  public service APIs only.
- `config_manager`: persistent application configuration owner.
- `wifi_manager`: Wi-Fi Station connection/reconnect owner.
- `provisioning_manager`: temporary BLE provisioning transport owner.
- `app_network_coordinator`: application network orchestration owner.
- `audio_manager`: sole microphone/speaker I2S, DMA, PCM-buffer, and
  playback/capture resource owner.
- `xiaozhi_foundation`: sole direct managed `esp_xiaozhi`/MCP provider boundary;
  provider handles and credentials do not escape.
- `voice_assistant`: product voice-session and recovery orchestration.
- `app_gui`: GUI screens/models/UI queues.
- `ui_manager_lvgl`: LVGL runtime/synchronization owner.
- `sd_card_manager`: SD lifecycle/lease owner.
- `light_manager`: product light state/effect owner; lower NeoPixel driver stays
  below this boundary.
- `components/system/common/include/board_config.h`: physical board mapping source
  of truth.

GPIO ownership retained from prior accepted project state:

```text
GPIO9   factory reset only
GPIO38  PTT input, active high, internal pull-down
GPIO48  NeoPixel reservation; never use as PTT
```

## Component portability hardening

The portability-hardening initiative was already merged into
`main_including_Firebase_security` by merge commit
`b7ef51a87dcefa330cd0aa42e4d52dafe60f2bba` and its agreed architecture scope
is frozen.

Preserve:

- dependency direction `application -> service -> driver/framework`;
- domain directories as organizational containers;
- parent-owned private `modules/<name>/` for tightly coupled subsystems;
- no cross-component inclusion of another component's private-module headers;
- runtime hardware configuration where previously introduced;
- product-specific application components remain product-specific unless a real
  reuse/lifecycle requirement justifies promotion/generalization.

Do not start another generic portability wave without a concrete validated
regression.

## Security/configuration invariants

- Firebase development values come from local generated configuration; never
  store real credentials in tracked source or `AI_Stored_Data/`.
- Never store Wi-Fi passwords, PoP values, private keys, service-account JSON,
  access tokens, activation secrets, or private transport payloads here.
- `.FireBaseKey` remains ignored by repository history.
- `log_manager` had earlier unbounded-copy cleanup; do not infer current build/
  HIL evidence from that historical source change.

## Documentation discrepancy

Current source and `AI_Stored_Data` show Sprint 18 in progress with Phase 18.1
implemented. At this synchronization point, `XIAOZHI_IMPLEMENTATION_ROADMAP.md`
still contains an older top-level status saying implementation is through Sprint
17 and a "Sprint 18 — Not Started" section.

Do not silently choose the stale roadmap status over current source. Preserve
this discrepancy and update the canonical roadmap in a dedicated documentation
step when requested.

## Current pending validation / technical debt

1. **PTT/TLS regression:** boot plus repeated PTT should confirm PSRAM TLS record
   allocation/headroom behavior after the current external-memory policy.
2. **Streaming regression:** confirm delayed first audio after TTS_START no longer
   causes a false prefill timeout, and verify normal playback/recovery remains
   audible.
3. Phase-16/16.1 endurance and broader full-Gateway integration remain deferred.
4. Phase-15 visible LCD/text coverage has historical partial gaps; rerun only
   when relevant to a regression.
5. Long-duration Firebase/cloud plus Xiaozhi simultaneous-traffic regression
   remains deferred.
6. Full post-portability target smoke/regression should be treated separately
   from architecture closure.

## Recommended next action

Do not start Phase 18.2 automatically. Recommended order from current source:

```text
clean build current HEAD
-> target boot + repeated PTT smoke
-> Phase-18.1 light HIL matrix
-> verify delayed-TTS/streaming behavior affected by 15cd0f06...
-> record evidence
-> reconcile canonical roadmap/docs
-> then consider 18.2 only if Hải explicitly starts it
```

Historical HIL branches remain regression baselines and should not be rewritten
merely to reflect newer production implementation.
