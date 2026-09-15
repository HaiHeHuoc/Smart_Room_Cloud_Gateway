# Xiaozhi Foundation

`xiaozhi_foundation` is the project-owned boundary for the production Xiaozhi
WebSocket session, voice audio channel, bounded semantic text bridge, and MCP
engine/tool lifecycle. Managed provider handles, credentials, tokens, transport
objects, and callback-lifetime framework pointers never leave the component.

## Production MCP tools

The following product tools are compiled and attached to a successfully started
production Xiaozhi/MCP session:

```text
smart_room.get_current_temperature_humidity
smart_room.get_cloud_sync_status
smart_room.get_system_status
cloud.push_latest
light.set_state
light.get_state
light.get_capabilities
audio.control_playback
audio.get_playback_state
audio.list_tracks
audio.play_track
audio.play_recorded
```

The light tool names intentionally use the current source contract
`light.*`; older documentation using `smart_room.light.*` is stale.

There is no Menuconfig switch for individual production MCP tools. Each tool is
bounded by its registered provider contract: query tools are read-only, and
`light.set_state` validates the complete bounded request before one delegated
logical-state operation.

Tool registration/attachment does not make tools usable while the Xiaozhi
session is stopped or disconnected.

## Provider Boundary

Application/domain providers are registered through
`smart_room_mcp_adapter` using the public `xiaozhi_foundation` provider APIs.

```text
smart_room_app
    -> smart_room_mcp_adapter
        -> xiaozhi_foundation_register_*_provider()
            -> xiaozhi_foundation MCP tool lifecycle
```

`smart_room_mcp_adapter` may read copied/public manager state and invoke bounded
public owner APIs, but it never receives or retains a managed MCP/Xiaozhi
handle.

## Lifecycle

`voice_assistant` owns product conversation orchestration and starts the
production session after required audio/arbitration state is ready.
`xiaozhi_foundation` owns the managed engine/session lifecycle: MCP tools attach
after MCP engine creation and detach before engine destruction.

The component does not own:

- I2S/DMA/audio hardware;
- LVGL/UI objects;
- Wi-Fi or provisioning lifecycle;
- cloud/Firebase ownership;
- project NVS policy;
- GPIO/RMT/NeoPixel resources.

## Controlled Action Ownership

For Phase 18.1, the production path is:

```text
Xiaozhi backend
-> xiaozhi_foundation `light.*` MCP tool
-> smart_room_mcp_adapter light provider
-> light_manager
-> NeoPixel hardware
```

`light_manager` remains the light-state/effect owner. MCP never drives GPIO/RMT
or the NeoPixel component directly.

For Phase 18.4, `cloud.push_latest {}` is a no-argument controlled action:

```text
Xiaozhi backend
-> xiaozhi_foundation cloud.push_latest tool
-> smart_room_mcp_adapter cloud provider
-> cloud_manager_request_push_latest()
-> existing cloud task/Firebase path
```

The foundation accepts neither telemetry values nor cloud endpoint/auth/HTTP
data. It delegates one copied scheduling result only. `accepted=true` always
means `upload_complete=false`; an MCP callback never waits for network work or
claims Firebase success. `not_ready`, `offline`, `busy`, `invalid_state`, and
`failed` remain bounded provider outcomes.

## Phase 18.2.2 Bounded Audio Selection

`audio.list_tracks` and `audio.play_track` are provider-bound tools, not a
filesystem API. The MCP layer accepts only an exact bounded logical `track_id`
and validates it again before calling the project adapter. The adapter's
dedicated worker scans `/sdcard/audio/` and publishes a bounded cache; no MCP
or Xiaozhi WebSocket callback performs catalog VFS I/O. The list result carries
a compact structured availability summary and a bounded text list containing
the exact ID, name, size, and deterministic first entry. Its reusable list
staging and output buffer use PSRAM rather than consuming the WebSocket stack
or persistent Internal RAM.

The adapter constructs a trusted internal path only after an exact cache match;
it never passes a model-supplied path to an audio API. Playback still flows
through `voice_assistant` policy, the existing arbiter and `audio_manager`,
which remains the sole I2S/DMA owner. `audio.play_recorded` uses only an
existing retained processed recording and cannot trigger capture. Successful
`audio.play_track` and `audio.play_recorded` results mean that a request was
accepted/scheduled; `playback_confirmed` remains `false` until a separate
manager-status or target-log observation proves playback actually began.

## Response-interruption transport fence

The pinned provider exposes a long-lived client generation but no server turn
or response ID, and raw WebSocket Opus frames carry no per-turn metadata. On a
local response abort, `xiaozhi_foundation_audio_abort_response()` first closes
the project response-delivery gate and sends the upstream abort while the
current audio session ID still exists. The next PTT capture cannot reuse that
transport: `voice_assistant` reserves a new generation and calls
`xiaozhi_foundation_session_rotate_transport()` from its normal Internal-RAM
lifecycle task.

The rotation retains the chat object and MCP engine/tool registrations, but
stops the old WebSocket task, posts a private FIFO drain marker on the same
event base, ignores old global connection events until the marker is observed,
then starts a fresh transport. Response delivery stays closed until the new
connection is READY and downlink has reserved a new local response epoch. It
opens immediately before that epoch's `stop-listening` transmit, so a fast
first TTS/Opus callback cannot race the synchronous send return; normal channel
cleanup, local abort, and every fence failure close it. A stop/start/drain
failure stays fail-closed and is reported as session ERROR; callers must not
authorize microphone capture in that case.

## Retired Validation Infrastructure

The Phase-12 temporary validation runtime, P2-F fixture paths,
lifecycle/fault matrices, resource-attribution tracing, and their Menuconfig
settings were removed during cleanup. Historical evidence remains in project
state records; it is not a runnable current production profile.
