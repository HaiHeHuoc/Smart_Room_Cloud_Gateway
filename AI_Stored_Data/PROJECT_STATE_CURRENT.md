# Smart Room Cloud Gateway — Current Project State

Updated: 2026-09-24
Active integration branch: `main_including_Firebase_security`
Sprint-18 closure authority: explicit user acceptance by Hải on 2026-09-16

> This file is the current-state companion for cross-session AI handoff.
> Current source, `AGENTS.md`, canonical repository documentation, and explicit
> build/HIL evidence remain higher authority for technical facts. The closure
> status below is an explicit project-management decision by Hải and must not be
> rewritten back to `IN PROGRESS` merely because some historical validation
> matrices remain useful as regression work.

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
Sprint 18   COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16
Phase 18.1  COMPLETE / build PASS / target HIL accepted by Hải on 2026-09-13
Phase 18.2  COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16
18.2.1      COMPLETE / playback control + PTT suspension/auto-resume
18.2.2      COMPLETE / bounded playback start + voice SD audio selection
Phase 18.3  COMPLETE / SUPERSEDED AND ABSORBED INTO 18.2.2 / NO DUPLICATE CODE
Phase 18.4  COMPLETE / cloud.push_latest / USER ACCEPTED BY HẢI ON 2026-09-16
Voice Recording Critical Window
            COMPLETE / USER ACCEPTED BY HẢI ON 2026-09-16
Sprint 19   Local Web Control V1: SD Card File Manager / SOURCE INTEGRATED /
            BUILD VERIFIED / TARGET HIL PARTIAL
Sprint 20   Local Web Control V2: Playback + Volume / IMPLEMENTED /
            BUILD VERIFIED / TARGET HIL PENDING
Sprint 21   Local Web Control V3: Lights / IMPLEMENTED / BUILD VERIFIED /
            TARGET HIL PENDING
Sprint 22   Local Web Control V4: Dashboard + System Status / PLANNED / NOT STARTED
Sprint 23   Local Web Control V5: Scenes + Logs + Diagnostics / PLANNED / NOT STARTED
Sprint 24   Wake Word + Advanced Voice UX / PLANNED / NOT STARTED
```

## Sprint 18 closure interpretation

Hải explicitly instructed the project to mark **all Phase 18 work from 18.1
through 18.4 complete** on 2026-09-16. This is the active project-management
status and supersedes older `IN PROGRESS`, `HIL PENDING`, and `READY TO CLOSE:
NO` status gates where they were being used to prevent phase closure.

This closure does **not** fabricate missing technical evidence. Preserve these
facts when discussing historical validation quality:

- Phase 18.1 has recorded build PASS and user-accepted target HIL.
- Phase 18.2.1/18.2.2 have recorded host-test/build evidence from their
  implementation checkpoints; older target-HIL matrices remain useful as
  optional regression coverage but are no longer phase blockers.
- Phase 18.3 is closed by reconciliation: its historical bounded playback-start
  scope is already implemented by 18.2.2, so no duplicate production feature is
  required.
- Phase 18.4 `cloud.push_latest` has recorded host policy/provider evidence. An
  earlier checkout-specific ESP-IDF build attempt was blocked by missing
  `IDF_PATH`; do not rewrite that historical event as a build PASS. Hải's
  explicit closure is the project acceptance decision.
- Voice Recording Critical Window has host state-machine evidence and is
  explicitly accepted complete by Hải; no extra target-HIL run should be
  invented.

Therefore future sessions must distinguish:

```text
PROJECT STATUS       = COMPLETE / ACCEPTED
RECORDED EVIDENCE    = whatever was actually observed and documented
OPTIONAL REGRESSION  = may still be run later without reopening Sprint 18
```

## Phase 18 delivered controlled-action surface

### 18.1 — Lights

Project-owned allowlisted light control remains routed through the Smart Room
provider/application boundary to `light_manager`; no arbitrary GPIO ownership
is exposed to MCP.

### 18.2 / 18.3 — Audio playback and bounded selection

Production MCP/control surface includes:

```text
audio.control_playback { action: pause | resume | stop | restart }
audio.get_playback_state {}
audio.list_tracks {}
audio.play_track { track_id: exact-id }
audio.play_recorded {}
```

Important retained contracts:

- `audio_manager` remains the sole physical audio/I2S owner;
- MCP receives copied/bounded state, never I2S/DMA handles or raw filesystem
  ownership;
- logical track IDs resolve only through the bounded catalog under the approved
  media root;
- local resumable playback and GPIO38 PTT coordination preserve explicit-user
  command precedence;
- stale response/generation work is fenced fail-closed.

Phase 18.3 remains historically numbered but is closed as **absorbed into
18.2.2** rather than implemented a second time.

### 18.4 — Cloud push

Production MCP surface:

```text
cloud.push_latest {}
```

Ownership path remains:

```text
xiaozhi_foundation tool/schema
-> smart_room_mcp_adapter provider
-> cloud_manager_request_push_latest()
-> existing cloud task/Firebase path
```

`accepted=true` means scheduling/retention of the request only, not proof of a
completed Firebase upload. Existing retry, auth, network-epoch, and cloud-task
ownership remain authoritative.

## Voice Recording Critical Window — COMPLETE

The project-owned `VOICE_RECORDING_CRITICAL` runtime contract is retained as a
cooperative workload-priority hint for the actual live microphone capture
interval:

- `voice_assistant_uplink` is the sole writer;
- entry occurs only after real capture/I2S RX activation;
- exit is generation-guarded;
- Wi-Fi/TCPIP/TLS/Xiaozhi/core audio tasks are not suspended;
- optional logging, monitor, DHT sampling, and ordinary periodic cloud work may
  defer at safe points;
- the bounded uplink queue is not enlarged to hide timing failures;
- one per-turn summary records timing/drop diagnostics without per-frame log
  spam.

This work item is closed by Hải's explicit acceptance on 2026-09-16.

## What happens next

Sprint 20 is now active at the Prompt 20.1 checkpoint. Do **not** automatically
start Prompt 20.2 or Sprint 21 merely from this state update.

Highest-value next validation is:

```text
1. Complete the remaining Sprint-19 storage HIL that is still unverified:
   upload through 20 MiB, interruption/partial cleanup, file/folder mutations,
   and SD removal/remount/recovery.
2. Run Sprint-20 Prompt-20.1 target HIL for browser playback/control, PTT and
   Xiaozhi arbitration, pause/resume/restart/stop, progress, volume 0/100,
   PC/mobile tabs, and storage/playback contention.
3. Use WAV files below 2 GiB for supported playback validation. FAT32 storage
   may list/download 2-4 GiB files, but the current WAV playback reader does not
   support files at or above 2 GiB.
```

The approved forward sequence remains Sprint 21 Lights, Sprint 22
Dashboard/System Status, Sprint 23 Scenes/Logs/Diagnostics, and Sprint 24 Wake
Word/Advanced Voice UX, but none starts automatically.

## Deferred regression work — non-blocking

The following may still be useful later but **must not reopen Sprint 18 merely
because they are pending**:

- combined audio/PTT/SD regression matrix from the former Phase-18.2 closure
  checklist;
- long-duration Phase-16/16.1 arbitration/streaming endurance;
- delayed-first-PCM focused regression;
- long-duration Firebase/cloud + Xiaozhi simultaneous traffic;
- Voice Recording Critical Window resource/timing trend measurements;
- release-level target smoke before a future tagged release if desired.

Only a concrete regression or explicit Hải instruction should reopen Sprint 18.

## Sprint 19 Local Web Storage V1 — source integrated / target HIL pending

- `local_web_server` is a presentation-only HTTP component over the existing
  SD manager; it does not own Wi-Fi, provisioning, SD mount/recovery, FATFS,
  LVGL, audio, or hardware resources.
- The Web surface has bounded browse, download, upload, file/folder mutations,
  centralized path validation, and copied LCD status routing. One Web operation
  runs at a time; uploads use a temporary file then atomic publication.
- FATFS capacity, signed FAT32 file sizes at or above 2 GiB, and browser
  download filenames are corrected. The maximum Web upload is 20 MiB.
- Host tests and a serialized ESP-IDF 6.0.1 build pass.
- Target HIL update (user-confirmed on 2026-09-22): the previously observed
  Web Storage capacity reporting defect and browser download defect are fixed
  on the deployed main revision. Capacity/status and download are therefore
  accepted for this HIL checkpoint.
- Remaining Sprint-19 target HIL still includes upload up to 20 MiB,
  interruption/partial-file cleanup, file/folder mutations, and SD
  removal/remount/recovery before full Sprint-19 acceptance.

## Sprint 20-21 Local Web controls — implemented / build verified / target HIL pending

- Sprint 20 extends the Storage frontend with owner-published audio catalog and
  playback state, catalog-ID playback, bounded volume, and generation-guarded
  seek. HTTP remains outside I2S, WAV-reader, and SD-lease ownership.
- Sprint 21 exposes the same product light state used by MCP through bounded
  Light REST routes and an accessible browser tab. Web and MCP both call only
  `light_manager`; no Web code accesses NeoPixel, RMT, or GPIO.
- Browser writes reconcile against read-back state. `app_gui` receives copied
  Local Web Light snapshots in a length-one queue and renders `WEB_LIGHT` only
  in its UI task. The status screen is read-only.
- Relevant host tests and ESP-IDF 6.0.1 serialized build passed. No browser,
  target-board, LCD, or multi-frontend HIL acceptance has yet been recorded.
### Historical Prompt 20.1 contract detail
## Sprint 20 Local Web Playback + Volume — Prompt 20.1 integrated

Verified source on `main_including_Firebase_security` includes the Prompt 20.1
implementation before this AI-memory synchronization.

Current Local Web audio surface:

```text
GET  /api/audio/status
GET  /api/audio/tracks
POST /api/audio/play
POST /api/audio/control
POST /api/audio/volume
```

Retained ownership/contracts:

- `audio_manager` remains the sole physical audio/I2S owner.
- Local Web uses the copied Smart Room catalog seam and logical track IDs only.
- Catalog invalidation occurs after committed storage mutation under
  `/audio`; Web does not create a second filesystem scanner/cache owner.
- Playback commands continue through the voice-assistant/audio arbitration path.
- Runtime playback volume is bounded to 0..100.
- Browser Storage and Playback tabs remain presentation-only frontends.

Recorded Prompt-20.1 evidence is host-test PASS across Local Web catalog/policy,
Smart Room catalog entry, and audio playback-control/WAV-lease paths, plus an
ESP-IDF 6.0.1 serialized build PASS. These are recorded implementation results;
this memory sync did not rerun them.

Current WAV playback supports files smaller than 2 GiB
(`<= 2,147,483,647` bytes) because the active FAT VFS/C stdio playback reader
uses signed 32-bit seek/tell positioning. This limit is independent of SD
capacity and the bounded PSRAM prefetch cache. Storage APIs may still
list/download FAT32 files in the 2-4 GiB range.

Target HIL for Prompt 20.1 remains pending.

## Security invariants

Never store real Wi-Fi credentials, Firebase passwords/API secrets, PoP values,
private keys, service-account JSON, access/refresh tokens, activation secrets,
or private transport payloads in tracked source or `AI_Stored_Data/`.
