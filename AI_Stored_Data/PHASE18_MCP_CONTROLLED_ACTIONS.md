# Phase 18 — MCP Controlled Actions Scope

Status: **IN PROGRESS — 18.1 IMPLEMENTED; CURRENT HEAD REVALIDATION + HIL PENDING**

Updated: 2026-09-12
Integration branch: `main_including_Firebase_security`
Observed remote source HEAD before this synchronization: `15cd0f06d25142a6ed7672bc99dfd4ec396184b0`

## Goal

Phase 18 introduces a small, allowlisted set of MCP actions that can create real
device-side side effects through existing project ownership boundaries. MCP is
an orchestration/interface layer only; it must not directly own GPIO/RMT,
NeoPixel, I2S, DMA, Wi-Fi, Firebase transport, LVGL, filesystems, or unrelated
lower-level resources.

This file records the approved Phase-18 scope plus the implementation truth that
future sessions need to resume safely.

## Approved scope

Phase 18 contains exactly four controlled-action slices:

```text
18.1  light.set_state
18.2  audio.stop_playback
18.3  audio.play_recorded / bounded allowlisted playback variant
18.4  cloud.push_latest
```

Do not add a fifth controlled action merely to increase feature count.

## 18.1 — NeoPixel light control

### Current implementation

Implemented tools:

```text
light.set_state
light.get_state
light.get_capabilities
```

`light.get_state` and `light.get_capabilities` are read-only companions to the
approved controlled action; they do not create additional Phase-18 controlled
actions.

The MCP-C-SDK-facing controlled request uses one required outer object:

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

At least one field is required. Unsupported fields or values are rejected
before the project-owned light provider is called.

### Current partial-update semantics at HEAD

Source at `15cd0f06...` defines these semantics:

- `power="off"` cannot be combined with color, brightness, or effect.
- Color-only and brightness-only requests preserve the current logical power.
- An effect request with no explicit power is an activation request and turns
  the logical light ON.
- If an effect is requested without an explicit color while the preserved RGB
  value is black, the composition adapter substitutes white `(255,255,255)` so
  the effect is visible.
- Other omitted fields preserve their current logical value.
- The composition adapter snapshots `light_manager`, builds one
  `light_manager_state_t`, performs one `light_manager_set_state()` operation,
  then copies the resulting logical state for the MCP response.
- Validation failures must produce no light-manager side effect.

### Manager-owned fixed effects

Current product timing is owned by `light_manager`:

```text
solid    static RGB at stored brightness
blink    500 ms ON / 500 ms OFF
breath   2000 ms period
pulse    1200 ms triangular pulse, repeated
rainbow  lower-layer single-LED rainbow cycle, 10 ms step
```

These timing values are implementation-owned product behavior. MCP does not
accept arbitrary timing or raw NeoPixel/RMT configuration.

Turning the light OFF darkens the LED while retaining copied logical RGB,
brightness, and effect. Subsequent valid activation uses the retained state
subject to the effect-only activation/default-white rule above.

### Ownership path

```text
User intent
-> Xiaozhi MCP light.set_state
-> bounded schema/allowlist validation
-> xiaozhi_foundation provider boundary
-> main-owned light composition adapter
-> light_manager
-> NeoPixel component / hardware
```

MCP must not include or call NeoPixel/RMT/GPIO APIs directly.

### Read-only companions

`light.get_state` returns copied logical power, RGB, brightness, effect, and a
bounded color name. Valid RGB values outside the Phase-18.1 named palette are
reported as `custom` with exact components.

`light.get_capabilities` is static/read-only and reports only the implemented
product contract: power, color, brightness, effect, ten named colors, five
fixed effects, and brightness range `0..100`. It has no hardware provider and
must not expose board/GPIO/RMT/task details.

## 18.1 validation history and current truth

Commit `f00e106150ddf2a48034a1ed9b6c6520aff20fc5` added the fixed effects and
capability query and explicitly recorded a full ESP-IDF build PASS. Target HIL
was still pending.

Two commits then changed source after that verified checkpoint:

1. `e0255881ad61a5bea4c96b49c866f20a0f8b3355`
   - reduced dynamic TLS outbound record size from 2 KiB to 1 KiB;
   - explicitly recorded a clean ESP-IDF build PASS;
   - target boot and repeated-PTT validation remained pending.

2. `15cd0f06d25142a6ed7672bc99dfd4ec396184b0`
   - changed Phase-18.1 effect request semantics and logging;
   - changed pulse duration from 300 ms to 1200 ms;
   - also changed voice uplink/TLS-memory and streaming-downlink behavior;
   - commit message contains no explicit build or HIL evidence.

Therefore do **not** claim current HEAD `15cd0f06...` is build-verified merely
because the earlier `f00e106...` checkpoint passed. Correct state is:

```text
18.1 implementation present       CONFIRMED
last explicit full Phase-18.1 build at f00e106...  PASS
current HEAD build after 15cd0f06...               PENDING / NOT RECORDED
current HEAD target HIL                              PENDING
```

## Minimum 18.1 HIL / regression matrix

Validate current HEAD before closing 18.1:

```text
pink, brightness 100%
green, brightness 20%
brightness 0%
brightness 100%
off
rapid color/brightness updates
solid
blink
breath
pulse (current 1200 ms product timing)
rainbow
effect-only request while light is OFF -> activates light
effect-only request from black RGB -> visible white fallback
power=off + color -> rejected
power=off + brightness -> rejected
power=off + effect -> rejected
light.get_state reflects applied logical state
light.get_capabilities reports current fixed contract
```

Because current HEAD also changes the Xiaozhi audio/TLS path, repeat at least a
small voice regression around the light HIL so a working LED command does not
hide a PTT/audio regression.

## 18.2 — Stop audio playback

Preferred product action:

```text
audio.stop_playback
```

Status: **NOT STARTED**.

Route through project-owned audio control and keep `audio_manager` as sole
I2S/playback owner. Report deterministic not-playing/busy/error/success results;
MCP must never touch I2S or DMA directly.

## 18.3 — Allowlisted audio playback

Preferred initial action:

```text
audio.play_recorded
```

Status: **NOT STARTED**.

A bounded notification-playback variant may be chosen if it better matches the
existing architecture. MCP must not accept arbitrary filesystem paths or raw
audio sources. Existing Phase-16 arbitration remains authoritative.

## 18.4 — Push latest cloud telemetry

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

A later phase or explicit roadmap decision is required for those capabilities.

## Architecture contract

Every controlled action follows:

```text
User intent
-> Xiaozhi / MCP tool
-> schema + allowlist validation
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

## Documentation discrepancy to preserve until fixed

At the time of this synchronization, current source and this handoff record
show Phase 18 in progress with 18.1 implemented, while
`XIAOZHI_IMPLEMENTATION_ROADMAP.md` still contains an older "Sprint 18 — Not
Started" status. Do not silently treat that stale status as current source
truth. Update the canonical roadmap in a dedicated documentation step when the
user requests/accepts that cleanup.

## Next action

Do not start 18.2 automatically. First revalidate current HEAD:

```text
clean ESP-IDF build
-> target boot / repeated PTT regression
-> Phase-18.1 light HIL matrix
-> update evidence
```

Only after that should Phase 18.1 be described as verified on the current HEAD.
