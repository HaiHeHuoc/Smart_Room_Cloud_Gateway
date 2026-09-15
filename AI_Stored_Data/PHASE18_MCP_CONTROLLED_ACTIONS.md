# Phase 18 — MCP Controlled Actions Scope

Status: **IN PROGRESS — 18.1 COMPLETE / 18.2 SOFTWARE INTEGRATED, TARGET HIL PENDING**

Updated: 2026-09-15
Integration branch: `main_including_Firebase_security`
Current integration source baseline before this AI-state synchronization: `b3b2e9b6f21ed355d6bdc7867ab184f73a4dd933` (`merge(audio): integrate Phase 18.2 playback and bounded selection`)

## Current-status override — 2026-09-15

This file originally described the pre-18.2 umbrella plan in which Phase 18.2
was `audio.stop_playback`, Phase 18.3 was the bounded playback start slice, and
Phase 18.4 was `cloud.push_latest`.

Later approved Phase-18.2 planning subdivided current audio work into:

```text
18.2.1  Audio Playback Control + PTT Suspension/Auto-Resume
18.2.2  Bounded Playback Start + Voice SD Audio Selection
```

That subdivision is now implemented and merged into the active integration
branch. Therefore any older sentence below or in historical notes saying
`Phase 18.2 NOT STARTED` is superseded for **execution status** by current source
and these newer records:

```text
AI_Stored_Data/PHASE18_2_PLAN.md
AI_Stored_Data/PHASE18_2_1_DESIGN_AUDIT.md
AI_Stored_Data/PHASE18_2_1_PROGRESS.md
AI_Stored_Data/PHASE18_2_2_PROGRESS.md
```

Current execution status is:

```text
18.1    NeoPixel MCP control                                  COMPLETE / HIL ACCEPTED
18.2.1  Audio Playback Control + PTT Suspension/Auto-Resume   SOFTWARE INTEGRATED / TARGET HIL PENDING
18.2.2  Bounded Playback Start + Voice SD Audio Selection     SOFTWARE INTEGRATED / TARGET HIL PENDING
18.3    NOT STARTED / NUMBERING AND PRIOR SCOPE PRESERVED
18.4    NOT STARTED / NUMBERING AND PRIOR SCOPE PRESERVED
```

Do not infer Phase-18.2 hardware acceptance from the merge/build/host tests.
Do not start, renumber, replace, or silently repurpose 18.3/18.4 from this
status synchronization. The older umbrella allocation below is retained as
historical planning context; if its wording conflicts with the newer locked
18.2 subdivision, use current source plus `PHASE18_2_PLAN.md` and explicitly
reconcile the remaining 18.3/18.4 scope before implementation.

`XIAOZHI_IMPLEMENTATION_ROADMAP.md` also still contains the older statement that
Phases 18.2-18.4 are not started. That is a known canonical-document discrepancy
to reconcile separately; it must not override the integrated 18.2 source.

## Goal

Phase 18 adds a small, allowlisted set of MCP actions that can create real
device-side side effects through existing ownership boundaries. MCP is an
orchestration/interface layer only; it must not directly own GPIO/RMT,
NeoPixel, I2S, DMA, Wi-Fi, Firebase transport, LVGL, filesystems, or unrelated
lower-level resources.

## Architecture contract

Every controlled action follows:

```text
User intent
-> Xiaozhi / MCP tool
-> bounded schema + allowlist validation
-> project-owned provider/action boundary
-> owning manager/service
-> real operation
-> bounded copied result
-> MCP response
```

Preserve these rules:

1. MCP never bypasses the owning manager/service.
2. Inputs are bounded and validated before side effects.
3. Unsupported values are rejected deterministically.
4. Busy/error/timeout behavior is explicit.
5. Repeated-delivery/idempotency behavior is defined for side effects.
6. Command acceptance is not reported as physical/service success unless the
   owner can prove it.
7. No LVGL call from an MCP callback.
8. No provider/driver/DMA/file handles, credentials, or private subsystem
   pointers escape their owners.
9. Phase-16 audio arbitration remains authoritative for audio ownership.
10. `audio_manager` remains the sole microphone/speaker I2S/DMA/playback owner.
11. `sd_card_manager` remains the SD/VFS lifecycle/lease owner.

# Phase 18.1 — NeoPixel light control — COMPLETE

## Accepted MCP surface

```text
light.set_state
light.get_state
light.get_capabilities
```

`light.set_state` accepts one required outer `state` object. Accepted nested
fields remain:

```text
power               "on" | "off"
color               red | green | blue | white | yellow | cyan |
                    magenta | pink | purple | orange
brightness_percent  integer 0..100
effect              solid | blink | breath | pulse | rainbow
```

Accepted partial-update semantics:

- `power="off"` cannot be combined with color, brightness, or effect;
- color-only and brightness-only preserve current logical power;
- an effect request without explicit power activates the light;
- if effect is requested without explicit color while preserved RGB is black,
  the adapter uses visible white `(255,255,255)`;
- other omitted fields preserve their logical values;
- validation failure produces no `light_manager` side effect.

Manager-owned fixed effects:

```text
solid    static RGB at stored brightness
blink    500 ms ON / 500 ms OFF
breath   2000 ms period
pulse    1200 ms triangular repeated pulse
rainbow  single-LED rainbow cycle, 10 ms step
```

Accepted ownership path:

```text
User intent
-> Xiaozhi backend
-> xiaozhi_foundation MCP tool / schema validation
-> smart_room_mcp_adapter provider
-> light_manager
-> NeoPixel component / hardware
-> copied logical result
-> MCP response
```

Phase-18.1 target HIL was accepted by Hải on 2026-09-13. The later application
structure cleanup and Phase-18.2 integration do not constitute a new Phase-18.1
target run and do not reopen 18.1 unless a concrete regression appears.

# Phase 18.2.1 — Audio Playback Control + PTT Suspension/Auto-Resume

Status: **SOFTWARE INTEGRATED / TARGET HIL PENDING**.

Current MCP surface:

```text
audio.control_playback { action: pause | resume | stop | restart }
audio.get_playback_state {}
```

Current integrated contract:

- bounded copied state/result only; no filesystem path, FILE pointer, raw PCM,
  I2S/DMA handle, SD lease, or private pointer crosses MCP/provider boundaries;
- `audio_manager` owns physical state, source identity, generation, committed
  position, cleanup, and I2S/DMA;
- local WAV and retained-recording sources are resumable;
- Xiaozhi live PCM/TTS is non-seekable and is cancelled/terminated rather than
  exact-resumed;
- GPIO38 PTT may temporarily suspend resumable local playback, wait finitely for
  a safe owner state, then carry the same still-held physical press into the
  normal PTT path;
- release before PTT authorization revokes the unstarted capture intent;
- temporary PTT suspension auto-resumes only after terminal voice response and
  only if no explicit audio-control action replaced that intent;
- explicit pause/resume/stop/restart wins over automatic resume;
- acceptance is distinct from physical completion and owner state is required
  to prove the latter.

Recorded software evidence:

```text
audio-manager host tests       PASS
voice-assistant host tests     PASS
Xiaozhi/provider host tests    PASS
ESP-IDF 6.0.1 build            PASS
target HIL                     PENDING / NOT CLAIMED
```

Detailed status: `AI_Stored_Data/PHASE18_2_1_PROGRESS.md`.

# Phase 18.2.2 — Bounded Playback Start + Voice SD Audio Selection

Status: **SOFTWARE INTEGRATED / TARGET HIL PENDING**.

Current MCP surface:

```text
audio.list_tracks {}
audio.play_track { track_id: exact-id }
audio.play_recorded {}
```

Current bounded catalog/start contract:

- media root is `/sdcard/audio/`;
- at most 12 direct bounded `.wav` entries are retained;
- logical IDs are deterministic and token-safe;
- unsafe names, arbitrary paths, directories, non-WAV and overlong/unknown IDs
  are rejected or excluded according to the bounded catalog policy;
- a persistent low-priority worker owns catalog scanning and holds an SD lease
  only while filesystem inspection is active;
- MCP/WebSocket callbacks use a copied cache and do not mount, unmount,
  initialize, or scan the SD card;
- catalog snapshot/worker resources and list staging use PSRAM where designed
  to preserve Internal RAM headroom;
- logical track selection resolves to an internal validated path and submits
  through existing playback arbitration and `audio_manager` ownership;
- `audio.play_recorded` reuses the existing retained processed recording and
  starts no new capture;
- `accepted` / `scheduled` means a request exists only and must not be presented
  as already-audible output.

The implementation also hardens local response cancellation with a generation
and transport fence so old queued/downlink/network work cannot silently enter a
new PTT response generation. Fence failure remains fail-closed.

Recorded software evidence:

```text
audio-manager host tests       PASS
voice-assistant host tests     PASS
Xiaozhi/provider host tests    PASS
ESP-IDF 6.0.1 build            PASS
firmware size                  0x26fb90
free app partition             0x190470 (39%)
DIRAM build-size snapshot      167376 / 341760 bytes (48.97%)
target HIL                     PENDING / NOT CLAIMED
```

Detailed status: `AI_Stored_Data/PHASE18_2_2_PROGRESS.md`.

## Phase 18.2 closure gate

Phase 18.2 is not closed. Combined target validation still needs to prove real
SD/I2S/audio/PTT/network behavior and resource stability, including:

- WAV play/pause/resume/restart/stop and repeated-cycle continuity;
- GPIO38 suspend -> same held press -> PTT -> response -> guarded auto-resume;
- explicit audio-command override and fast-release behavior;
- GPIO38 interruption during Xiaozhi TTS;
- valid/invalid/empty/over-limit track catalogs;
- SD unavailable/remount/media-error recovery;
- list/play races with catalog scanning;
- repeated response abort/transport-fence cycles without stale speech/work in
  the next generation;
- Internal/DMA/PSRAM minima/largest blocks, task stack high-water marks, CPU,
  SD lease trend, and control/play-start latency.

Until target evidence is recorded:

```text
PHASE 18.2 READY TO CLOSE: NO
```

# Historical umbrella allocation for remaining Phase 18 scope

The pre-18.2 umbrella plan recorded these later actions:

```text
audio.play_recorded / bounded allowlisted playback variant
cloud.push_latest
```

The bounded playback-start capability is now present inside the locked 18.2.2
implementation. Therefore do not mechanically reuse this old allocation to
implement a duplicate Phase 18.3. The current `PHASE18_2_PLAN.md` explicitly
requires the numbering and prior 18.3/18.4 scope to be preserved, but a future
session must reconcile the remaining canonical scope before starting 18.3.

The historical Phase-18.4 direction was `cloud.push_latest`: request a bounded
latest telemetry operation through `cloud_manager`, never expose Firebase/auth/
HTTP/task internals, and distinguish asynchronous request acceptance from
actual upload completion. This remains **NOT STARTED** and must not be started
without Hải's explicit request and a current scope check.

## Explicitly out of scope

Phase 18 does not authorize:

- `network.reconnect`;
- display-status/brightness features merely to increase MCP count;
- fan or servo control;
- arbitrary GPIO;
- arbitrary filesystem-path playback;
- shell/system commands;
- arbitrary task control;
- factory reset or credential erase;
- arbitrary reboot;
- arbitrary OTA/update commands.

## Next action

Highest-value unfinished Sprint-18 work is the combined Phase-18.2 target HIL
and resource validation recorded in `NEXT_WORK_AND_HIL_BACKLOG.md`.

Do not start Phase 18.3, Phase 18.4, or any Sprint 19-24 implementation
automatically. Start new scope only when Hải explicitly requests it.
