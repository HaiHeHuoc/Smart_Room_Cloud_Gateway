# Phase 18 — MCP Controlled Actions Scope

Status: **IN PROGRESS — 18.1 COMPLETE / BUILD PASS / TARGET HIL ACCEPTED**

Updated: 2026-09-13
Integration branch: `main_including_Firebase_security`
Current production/source baseline before documentation-only synchronization: `0a8c83f7776d8259f22208a66f7fc4bd52156aff`

## Goal

Phase 18 adds a small, allowlisted set of MCP actions that can create real
device-side side effects through existing ownership boundaries. MCP is an
orchestration/interface layer only; it must not directly own GPIO/RMT,
NeoPixel, I2S, DMA, Wi-Fi, Firebase transport, LVGL, filesystems, or unrelated
lower-level resources.

## Approved scope

Phase 18 contains exactly four controlled-action slices:

```text
18.1  light.set_state
18.2  audio.stop_playback
18.3  audio.play_recorded / bounded allowlisted playback variant
18.4  cloud.push_latest
```

`light.get_state` and `light.get_capabilities` are read-only companions to 18.1,
not extra controlled-action slices. Do not add a fifth action merely to increase
feature count.

# Phase 18.1 — NeoPixel light control — COMPLETE

## Accepted MCP surface

```text
light.set_state
light.get_state
light.get_capabilities
```

`light.set_state` accepts one required outer `state` object, for example:

```json
{"state":{"power":"on","color":"pink","brightness_percent":100}}
```

Accepted nested fields:

```text
power               "on" | "off"
color               red | green | blue | white | yellow | cyan |
                    magenta | pink | purple | orange
brightness_percent  integer 0..100
effect              solid | blink | breath | pulse | rainbow
```

At least one field is required. Unsupported fields/values and invalid field
combinations are rejected before the application provider calls the owning
manager.

## Accepted partial-update semantics

- `power="off"` cannot be combined with color, brightness, or effect.
- color-only and brightness-only requests preserve current logical power.
- an effect request with no explicit power activates the light.
- if an effect is requested without explicit color while preserved RGB is
  black, the application adapter substitutes white `(255,255,255)` so the
  effect is visible.
- other omitted fields preserve their logical values.
- the provider snapshots `light_manager`, builds one requested logical state,
  applies it through `light_manager_set_state()`, then copies the applied
  logical state for the MCP result.
- validation failure produces no `light_manager` side effect.

## Manager-owned fixed effects

```text
solid    static RGB at stored brightness
blink    500 ms ON / 500 ms OFF
breath   2000 ms period
pulse    1200 ms triangular repeated pulse
rainbow  lower-layer single-LED rainbow cycle, 10 ms step
```

MCP does not accept arbitrary timing or raw NeoPixel/RMT configuration.

## Current ownership path after application-structure cleanup

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

`smart_room_mcp_adapter` replaces the old loose `main/xiaozhi_*_composition.*`
layout. It does not own the MCP engine/session or the light hardware. Managed
Xiaozhi handles remain inside `xiaozhi_foundation`; product light state/effects
remain inside `light_manager`.

## Read-only companions

`light.get_state` returns copied logical power, RGB, brightness, effect, and a
bounded color name. RGB values outside the named Phase-18.1 palette may be
reported as `custom` with exact components.

`light.get_capabilities` reports only the fixed product contract: power, color,
brightness, effect, ten named colors, five fixed effects, and brightness range
`0..100`. It exposes no board/GPIO/RMT/task details.

## Acceptance evidence

Historical implementation checkpoints:

```text
f00e106...  fixed effects/capabilities; full ESP-IDF build PASS; HIL pending then
e0255881... TLS outbound-record reduction; clean build PASS; target regression pending then
15cd0f06... final Phase-18.1 semantics plus voice/TLS/streaming changes
```

Subsequent evidence recorded on 2026-09-13:

```text
Phase-18.1 implementation                CONFIRMED
relevant current-source build            PASS
Firebase boot                            PASS — user confirmed
repeated PTT/TLS smoke                   PASS — user confirmed
Phase-18.1 light HIL matrix              PASS — user confirmed
Phase-18.1 closure                       ACCEPTED BY USER
```

Accepted light matrix includes:

```text
pink 100%
green 20%
brightness 0%
brightness 100%
off
rapid color/brightness updates
solid
blink
breath
pulse (1200 ms product timing)
rainbow
effect-only while OFF -> activates light
effect-only with preserved black RGB -> visible white fallback
power=off + color -> rejected
power=off + brightness -> rejected
power=off + effect -> rejected
light.get_state matches applied logical state
light.get_capabilities matches the fixed contract
```

The later source-structure cleanup at `0a8c83f...` was intended to preserve
behavior and recorded a normal ESP-IDF build PASS. No new target HIL was run
specifically after the structural move. Do not relabel the earlier accepted HIL
as a post-cleanup hardware run; Phase 18.1 nevertheless remains closed unless a
concrete regression is found.

The delayed-first-PCM streaming regression and long-duration PTT/resource
endurance are separate deferred work and do not reopen Phase 18.1.

# Phase 18.2 — Stop audio playback

Preferred product action:

```text
audio.stop_playback
```

Status: **NOT STARTED**.

Route through project-owned audio control and keep `audio_manager` as sole
I2S/playback owner. Report deterministic not-playing/busy/error/success results;
MCP must never touch I2S or DMA directly.

# Phase 18.3 — Allowlisted audio playback

Preferred initial action:

```text
audio.play_recorded
```

Status: **NOT STARTED**.

A bounded notification-playback variant may be chosen if it better matches the
existing architecture. MCP must not accept arbitrary filesystem paths or raw
audio sources. Existing Phase-16 arbitration remains authoritative.

# Phase 18.4 — Push latest cloud telemetry

Preferred product action:

```text
cloud.push_latest
```

Status: **NOT STARTED**.

Request the latest bounded telemetry snapshot through `cloud_manager`. If a new
API is necessary, add the smallest project-owned request API; do not expose
Firebase/auth/HTTP/task internals. Distinguish request acceptance from actual
upload completion when asynchronous.

## Explicitly out of scope

Phase 18 does not include:

- `network.reconnect`;
- display-status/brightness features merely to increase MCP count;
- fan or servo control;
- arbitrary GPIO;
- arbitrary WAV/file-path playback;
- shell/system commands;
- arbitrary task control;
- factory reset or credential erase;
- arbitrary reboot;
- arbitrary OTA/update commands.

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

## Next action

Phase 18.1 is already closed. Do not rerun its acceptance as a prerequisite for
normal continuation unless a regression is suspected.

Do not start Phase 18.2 automatically. Start it only when Hải explicitly
requests it.
