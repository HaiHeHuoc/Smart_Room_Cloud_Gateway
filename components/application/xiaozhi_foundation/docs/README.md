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
light.set_state
light.get_state
light.get_capabilities
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

## Retired Validation Infrastructure

The Phase-12 temporary validation runtime, P2-F fixture paths,
lifecycle/fault matrices, resource-attribution tracing, and their Menuconfig
settings were removed during cleanup. Historical evidence remains in project
state records; it is not a runnable current production profile.
