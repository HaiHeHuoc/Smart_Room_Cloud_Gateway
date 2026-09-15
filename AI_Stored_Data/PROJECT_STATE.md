# Smart Room Cloud Gateway — AI Project State

Updated: 2026-09-15
Active integration branch: `main_including_Firebase_security`
Current integration source baseline before this AI-state synchronization: `b3b2e9b6f21ed355d6bdc7867ab184f73a4dd933` (`merge(audio): integrate Phase 18.2 playback and bounded selection`)

## Working authority

Use this priority when resuming work:

1. Current source/build configuration on the active branch.
2. `AGENTS.md` and current canonical repository documentation.
3. Explicit recent build/HIL/manual evidence from Hải.
4. Current `AI_Stored_Data/` handoff notes.
5. Historical records, conversation memory, or assumptions.

`AI_Stored_Data/` is cross-session support metadata only. Production firmware,
CMake, Kconfig, tests, and runtime code must never depend on it.

## Current application structure

The application-structure cleanup remains integrated:

```text
main/main.c
    -> smart_room_app
        -> product startup/order/policy and copied callback routing
        -> smart_room_mcp_adapter
            -> Smart Room provider adaptation
            -> xiaozhi_foundation
                -> managed esp_xiaozhi / MCP engine and session
```

Key ownership boundaries:

- `main`: thin ESP-IDF entrypoint.
- `smart_room_app`: product composition, startup ordering, application policy,
  and copied cross-component callback routing.
- `smart_room_mcp_adapter`: project-owned MCP/provider adaptation using public
  service APIs only.
- `xiaozhi_foundation`: sole direct managed `esp_xiaozhi` / MCP engine/session
  boundary.
- `voice_assistant`: product voice-session, PTT, recovery, and turn-policy
  orchestration.
- `audio_manager`: sole microphone/speaker I2S, DMA, PCM, recording/playback
  resource owner.
- `sd_card_manager`: SD/VFS lifecycle and lease owner.
- `light_manager`: product light state/effect owner.
- `sensor_manager`, `cloud_manager`, `wifi_manager`, `config_manager`,
  `app_gui`, `ui_manager_lvgl`, and other managers retain their established
  domain ownership.
- dependency direction remains `application -> service -> driver/framework`.

## Current high-level phase state

```text
Sprint 12   COMPLETE / HIL PASS
Sprint 13   COMPLETE / HIL PASS
Sprint 14   SOFTWARE COMPLETE / BUILD PASS / golden-path HIL PASS / targeted regression partial
Sprint 15   COMPLETE / BUILD VERIFIED / HIL ACCEPTED
Sprint 16   COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Phase 16.1  COMPLETE BASELINE / streaming HIL accepted / endurance pending
Sprint 17   COMPLETE / read-only MCP voice HIL accepted
Sprint 18   MCP CONTROLLED ACTIONS / IN PROGRESS
Phase 18.1  COMPLETE / BUILD PASS / target HIL accepted by Hải on 2026-09-13
Phase 18.2  SOFTWARE INTEGRATED / TARGET HIL PENDING
18.2.1      Audio Playback Control + PTT Suspension/Auto-Resume
            SOFTWARE IMPLEMENTED / BUILD + HOST TESTS VERIFIED / TARGET HIL PENDING
18.2.2      Bounded Playback Start + Voice SD Audio Selection
            SOFTWARE IMPLEMENTED / BUILD + HOST TESTS VERIFIED / TARGET HIL PENDING
Phase 18.3  NOT STARTED / scope preserved
Phase 18.4  NOT STARTED / scope preserved
Sprint 19   Local Web Control V1: SD Card File Manager / PLANNED / NOT STARTED
Sprint 20   Local Web Control V2: Playback + Volume / PLANNED / NOT STARTED
Sprint 21   Local Web Control V3: Lights / PLANNED / NOT STARTED
Sprint 22   Local Web Control V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23   Local Web Control V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24   Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

Phase-18.2 source was integrated into the active branch by merge commit
`b3b2e9b6f21ed355d6bdc7867ab184f73a4dd933` on 2026-09-15. The merge is not
hardware acceptance. Phase 18.2 remains open until target HIL evidence is
recorded.

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

Accepted semantics remain:

- `power=off` cannot be combined with color, brightness, or effect;
- color-only and brightness-only preserve logical power;
- effect without explicit power activates the light;
- effect without explicit color uses white when preserved RGB is black;
- invalid requests produce no `light_manager` side effect;
- fixed effect timing remains blink 500/500 ms, breath 2000 ms, pulse 1200 ms,
  rainbow 10 ms step.

Phase 18.1 target HIL was accepted by Hải on 2026-09-13. The later structural
cleanup and Phase-18.2 work do not create a new Phase-18.1 target run and do not
reopen 18.1 unless a concrete regression is found.

## Phase 18.2.1 — Audio playback control + PTT suspension/auto-resume

Current MCP surface:

```text
audio.control_playback { action: pause | resume | stop | restart }
audio.get_playback_state {}
```

Current integrated behavior:

- `audio_manager` owns the physical playback state, source identity,
  generation, committed position, cleanup, and sole I2S/DMA access.
- Local WAV and retained-recording playback are resumable.
- Xiaozhi live PCM/TTS is non-seekable; pause/resume/restart are not supported
  for that source and the old response is cancelled/terminated instead.
- GPIO38 PTT can temporarily suspend resumable local playback, wait finitely for
  the owner to reach a safe paused condition, and carry the same still-held
  physical press into the normal PTT path.
- Releasing GPIO38 before authorization revokes the unstarted capture intent.
- After the voice response reaches terminal state, temporary PTT suspension is
  auto-resumed only if no explicit audio-control action replaced it.
- Explicit pause/resume/stop/restart during the turn wins over default
  auto-resume and is sequenced after TTS to avoid unnecessary overlap.
- MCP/provider results remain bounded copied state; no path, FILE, raw PCM,
  I2S/DMA handle, SD lease, or private pointer escapes its owner.

Recorded validation for the implementation checkpoints:

- audio-manager host tests: PASS;
- voice-assistant host tests: PASS;
- Xiaozhi/provider boundary host tests: PASS;
- ESP-IDF 6.0.1 build: PASS;
- target HIL: **NOT RUN / PENDING**.

Detailed records:

```text
AI_Stored_Data/PHASE18_2_1_DESIGN_AUDIT.md
AI_Stored_Data/PHASE18_2_1_PROGRESS.md
```

## Phase 18.2.2 — Bounded playback start + voice SD audio selection

Current MCP surface:

```text
audio.list_tracks {}
audio.play_track { track_id: exact-id }
audio.play_recorded {}
```

Current bounded catalog/playback policy:

- dedicated media root: `/sdcard/audio/`;
- at most 12 retained tracks;
- eligible entries are bounded direct `.wav` files only;
- deterministic token-safe logical IDs are used; safe display names may remain
  human-readable;
- arbitrary model-supplied filesystem paths are never accepted;
- catalog scans are non-recursive and owned by a persistent low-priority worker;
- only the worker holds an SD lease while `opendir`/`readdir`/`stat` work is in
  progress;
- MCP/WebSocket callbacks use zero-wait bounded cache copies/lookups and do not
  mount, unmount, initialize, or scan the card;
- catalog snapshot, worker stack, and MCP list staging are placed in PSRAM where
  designed to preserve Internal RAM headroom;
- track requests resolve logical IDs to a validated internal path and then use
  the existing playback arbiter and `audio_manager` owner;
- `audio.play_recorded` reuses the retained processed recording only and starts
  no new capture;
- `accepted` / `scheduled` means a request exists, not that speaker playback has
  already been physically confirmed.

The stability hardening also uses response-generation gating and a transport
fence after local response aborts so stale queued/downlink/network work cannot
become the next PTT generation. Fence failure remains fail-closed.

Recorded validation for the implementation checkpoint:

- audio-manager / voice-assistant / Xiaozhi host suites: PASS;
- ESP-IDF 6.0.1 `ninja -C build -j 1 all`: PASS;
- firmware size: `0x26fb90`; free app partition `0x190470` (39%);
- DIRAM from `ninja -C build size`: `167376 / 341760` bytes (48.97%);
- target HIL: **NOT RUN / PENDING**.

Detailed records:

```text
AI_Stored_Data/PHASE18_2_PLAN.md
AI_Stored_Data/PHASE18_2_2_PROGRESS.md
```

## Phase 18.2 target closure remains pending

Required hardware/system validation includes, at minimum:

- WAV play/pause/resume/restart/stop and repeated-cycle continuity;
- pause at multiple positions and resume within the documented committed-block
  tolerance;
- GPIO38 playback suspension -> same held press -> PTT -> response -> guarded
  auto-resume;
- explicit pause/resume/stop/restart override during the voice turn;
- fast GPIO38 release and GPIO38 interruption during Xiaozhi TTS;
- valid/unknown tracks, empty/over-limit catalog, non-WAV and unsupported WAV;
- SD unavailable/remount/recovery, media-error invalidation, and list/play while
  catalog scanning;
- repeated local response abort/transport-fence cycles with no stale speech or
  stale response entering the next PTT generation;
- Internal/DMA/PSRAM free/min/largest, relevant task stack HWM, CPU, SD lease
  trend, and playback/authorization latency.

Until target evidence is recorded:

```text
PHASE 18.2 READY TO CLOSE: NO
```

## Canonical roadmap discrepancy

`XIAOZHI_IMPLEMENTATION_ROADMAP.md` currently still says Phases 18.2-18.4 are
not started. That status is older than the actual merged source and the newer
Phase-18.2 planning/progress files.

For current execution status, source plus the newer Phase-18.2 records take
precedence. Preserve the discrepancy visibly and reconcile the canonical
roadmap in a separate documentation task. Do not invent, renumber, or silently
repurpose the preserved Phase 18.3/18.4 scope while doing so.

## Approved roadmap after Sprint 18

Future allocation remains:

```text
Sprint 19  Local Web Control V1: SD Card File Manager
Sprint 20  Local Web Control V2: Playback + Volume
Sprint 21  Local Web Control V3: Lights
Sprint 22  Local Web Control V4: Dashboard + System Status
Sprint 23  Local Web Control V5: Scenes + Logs + Diagnostics
Sprint 24  Wake Word + Advanced Voice UX
```

The Local Web roadmap is SD-card-first. Web UI is used after the device is
already networked; it must not configure/control Wi-Fi, provisioning,
credentials, reconnect, or network lifecycle. Web and LCD remain sibling
frontends over existing manager/service APIs. Advanced OTA and factory
management remain outside the current scope.

## Voice / TLS / streaming retained facts

The current accepted voice path still uses:

- dynamic mbedTLS buffers in PSRAM;
- 1 KiB outbound TLS record;
- a PTT gate requiring both total and largest-contiguous PSRAM headroom of at
  least 20 KiB;
- a 7.68-second bounded PSRAM ingress ring;
- 0.96-second normal streaming prefill;
- a 5-second prefill wait that starts after the first PCM packet, not at
  `TTS_START`.

The delayed-first-PCM regression and longer endurance/resource-trend work remain
separate deferred validation. Do not claim them PASS without newer target
records.

## Current deferred work outside Phase 18.2 closure

1. delayed-first-PCM / streaming regression on the current voice path;
2. Phase-16/16.1 endurance and long-duration resource trend checks;
3. long-duration Firebase/cloud + Xiaozhi simultaneous-traffic regression;
4. relevant UI regression only when a future defect touches that path;
5. bounded post-structure-cleanup target smoke if desired before a release
   checkpoint.

## Recommended next action

The highest-value unfinished work is **Phase 18.2 combined target HIL and
resource validation**. Do not mark 18.2 complete from merge/build/host tests
alone.

Do not start Phase 18.3, Phase 18.4, or Sprint 19-24 automatically. Start new
implementation only when Hải explicitly requests the relevant scope.

## Security invariants

Never store real Wi-Fi credentials, Firebase passwords/API secrets, PoP values,
private keys, service-account JSON, access/refresh tokens, activation secrets,
or private transport payloads in tracked source or `AI_Stored_Data/`.
