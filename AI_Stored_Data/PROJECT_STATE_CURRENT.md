# Smart Room Cloud Gateway — Current Project State

Updated: 2026-09-15
Active integration branch: `main_including_Firebase_security`
Current integration HEAD before this AI-state synchronization: `b3b2e9b6f21ed355d6bdc7867ab184f73a4dd933` (`merge(audio): integrate Phase 18.2 playback and bounded selection`)

> This file is the current-state companion for cross-session AI handoff.
> Current source, `AGENTS.md`, canonical repository documentation, and explicit
> build/HIL evidence remain higher authority.

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
- `sd_card_manager`: SD/VFS mount and lease owner.
- `light_manager`: product light state/effect owner; NeoPixel remains below it.
- `sensor_manager`, `cloud_manager`, `wifi_manager`, `config_manager`,
  `app_gui`, and other managers retain their established ownership.
- dependency direction remains `application -> service -> driver/framework`.

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

The Phase-18.2 implementation was merged into the integration branch by
`b3b2e9b6...` on 2026-09-15. The merge is source-integration evidence only; it
does not convert the pending target HIL into acceptance.

## Phase 18.2.1 integrated contract

Production MCP/control surface:

```text
audio.control_playback { action: pause | resume | stop | restart }
audio.get_playback_state {}
```

Key semantics:

- MCP reports bounded copied state and never exposes I2S/DMA handles, raw PCM,
  FILE pointers, filesystem paths, or SD ownership objects.
- `audio_manager` owns physical playback state and source context.
- Local WAV and retained-recording sources are resumable; Xiaozhi live PCM/TTS
  remains non-seekable and is cancelled/terminated rather than resumed.
- GPIO38 PTT uses the same project-owned control seam locally: resumable local
  playback can be temporarily paused with PTT reason, the same still-held press
  continues into capture after bounded suspension, and fast release revokes an
  unstarted turn.
- After the voice response terminates, temporary PTT suspension auto-resumes
  only if no explicit audio action replaced that intent. Explicit
  pause/resume/stop/restart wins.
- Acceptance of a command is not reported as physical completion unless copied
  owner state proves the applied result.

## Phase 18.2.2 integrated contract

Production MCP surface:

```text
audio.list_tracks {}
audio.play_track { track_id: exact-id }
audio.play_recorded {}
```

Bounded catalog policy:

- media root is `/sdcard/audio/`;
- maximum 12 retained tracks;
- only bounded direct `.wav` entries are eligible;
- logical IDs are deterministic and token-safe;
- MCP receives logical IDs/names/metadata only, never arbitrary raw paths;
- the catalog worker is the only scanner and holds the SD lease only while
  `opendir`/`readdir`/`stat` work is in progress;
- MCP/WebSocket callbacks use bounded cached lookup and do not mount, unmount,
  initialize, or scan the card;
- catalog snapshot/worker resources and list staging use PSRAM where designed to
  preserve Internal RAM for TLS/I2S/transport work;
- `audio.play_track` resolves an approved logical ID to an internal validated
  path and submits through the existing playback arbitration/`audio_manager`
  ownership path;
- `accepted` / `scheduled` means a request exists, not that audible playback is
  already confirmed.

The Phase-18.2 stability hardening also adds generation/response fencing around
locally aborted Xiaozhi responses so stale queued/network response work cannot
silently authorize the next PTT turn. A failed fence remains fail-closed.

## Validation evidence currently recorded

### 18.2.1

- host audio-manager tests: PASS;
- host voice-assistant tests: PASS;
- host Xiaozhi/provider boundary tests: PASS;
- ESP-IDF 6.0.1 build: PASS on the implementation checkpoint;
- target HIL: **NOT RUN / PENDING**.

### 18.2.2

- audio-manager, voice-assistant, and Xiaozhi host suites: PASS after stability
  hardening;
- ESP-IDF 6.0.1 `ninja -C build -j 1 all`: PASS on the implementation
  checkpoint;
- recorded firmware size: `0x26fb90`, free app partition `0x190470` (39%);
- recorded `ninja -C build size` DIRAM: `167376 / 341760` bytes (48.97%);
- target HIL: **NOT RUN / PENDING**.

Do not reinterpret feature-branch build/host-test evidence as a separate
post-merge target run. No audible output, real SD removal/recovery, I2S timing,
TLS timing, or long-duration resource stability is accepted for Phase 18.2
until target evidence is recorded.

## Required Phase 18.2 target closure direction

At minimum, combined target validation still needs to cover:

- WAV play/pause/resume/restart/stop and repeated-cycle continuity;
- GPIO38 suspend -> same held press -> PTT -> response -> guarded auto-resume;
- explicit audio-command override during a voice turn;
- fast GPIO38 release and GPIO38 interruption during Xiaozhi TTS;
- valid/unknown tracks, empty/over-limit catalog, unsupported/non-WAV entries;
- SD unavailable/remount/recovery and media-error snapshot invalidation;
- catalog scan races with list/play/PTT admission;
- repeated response interruption/transport-fence cycles with no stale speech or
  stale response entering the next generation;
- Internal/DMA/PSRAM free/min/largest, relevant task stack HWM, CPU, SD lease
  trend, and playback/authorization latency.

Until this evidence is recorded:

```text
PHASE 18.2 READY TO CLOSE: NO
```

## Canonical-document discrepancy to preserve visibly

`XIAOZHI_IMPLEMENTATION_ROADMAP.md` still records Phases 18.2-18.4 as not
started. That status is older than the merged Phase-18.2 source and the newer
`PHASE18_2_PLAN.md`, `PHASE18_2_1_PROGRESS.md`, and
`PHASE18_2_2_PROGRESS.md` records.

For current execution status, use the actual integration source and newer
Phase-18.2 records. Do **not** silently invent or renumber the preserved Phase
18.3/18.4 scope while reconciling the canonical roadmap later.

## Approved post-Sprint-18 roadmap

The approved future sequence remains:

```text
19  Local Web Control V1: SD Card File Manager
20  Local Web Control V2: Playback + Volume
21  Local Web Control V3: Lights
22  Local Web Control V4: Dashboard + System Status
23  Local Web Control V5: Scenes + Logs + Diagnostics
24  Wake Word + Advanced Voice UX
```

The Web roadmap is SD-card-first. Web UI is for an already-networked device and
must not configure/control Wi-Fi, provisioning, credentials, reconnect, or
network lifecycle. Web and LCD remain sibling frontends over existing managers
and services. Advanced OTA/factory-management remains outside current scope.

## Deferred work outside Phase 18.2 closure

- delayed-first-PCM / streaming regression on the current voice path;
- Phase-16/16.1 endurance and long-duration resource trend checks;
- long-duration Firebase/cloud + Xiaozhi simultaneous-traffic regression;
- bounded post-structure-cleanup target smoke if desired for a release
  checkpoint.

Do not start Phase 18.3, Phase 18.4, or Sprint 19-24 automatically. Start new
implementation only when Hải explicitly requests the relevant scope.

## Security invariants

Never store real Wi-Fi credentials, Firebase passwords/API secrets, PoP values,
private keys, service-account JSON, access/refresh tokens, activation secrets,
or private transport payloads in tracked source or `AI_Stored_Data/`.
