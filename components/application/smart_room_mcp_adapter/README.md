# Smart Room MCP Adapter

`smart_room_mcp_adapter` is the product/application bridge between Smart Room
public service APIs and provider callbacks registered with
`xiaozhi_foundation`.

Its single public lifecycle API,
`smart_room_mcp_adapter_register_providers()`, is called by `smart_room_app`
after local services are ready and before production voice startup.

## Responsibility

The component consolidates domain-provider adaptation that previously lived as
loose application composition files. It converts copied/public service state
into bounded `xiaozhi_foundation` provider results and delegates controlled
operations to the owning manager.

Current provider coverage supports the production tools:

```text
smart_room.get_current_temperature_humidity
smart_room.get_cloud_sync_status
smart_room.get_system_status
light.set_state
light.get_state
light.get_capabilities
```

Tool definition, schema parsing, MCP engine/session lifecycle, and managed
Xiaozhi handles remain inside `xiaozhi_foundation`; this adapter supplies the
project/domain side of those provider contracts.

## Ownership Path

```text
smart_room_app
    -> smart_room_mcp_adapter_register_providers()
        -> xiaozhi_foundation_register_*_provider()

runtime tool call
    -> xiaozhi_foundation tool/schema validation
    -> registered smart_room_mcp_adapter provider
    -> owning public manager/service API
    -> copied bounded result
```

## Boundaries

- Providers read copied/public state from the appropriate owners, including
  `sensor_manager`, `cloud_manager`, `time_manager`, `sd_card_manager`,
  `audio_manager`, and `light_manager` where required by the current provider.
- Light providers invoke only the public `light_manager` logical-state API;
  they never access GPIO, RMT, NeoPixel handles, or board mapping.
- `xiaozhi_foundation` remains the sole direct `esp_xiaozhi`/MCP engine and
  session owner. It attaches tools after MCP engine creation and detaches them
  before engine destruction.
- Provider function/context registrations are borrowed for firmware lifetime.
  This component never receives or retains a managed Xiaozhi/MCP handle.
- The adapter does not own LVGL, I2S/DMA, Wi-Fi/provisioning, Firebase
  transport, storage lifecycle, or hardware drivers.

## Phase 18.1

The accepted light-control path is:

```text
light.set_state
-> xiaozhi_foundation bounded validation
-> smart_room_mcp_adapter light provider
-> light_manager
-> NeoPixel hardware
```

`light.get_state` and `light.get_capabilities` are read-only companions.
Phase 18.1 is complete and accepted; Phase 18.2-18.4 remain not started.

The application-structure cleanup only consolidated existing provider logic. It
added no new MCP tool, transport/protocol behavior, hardware ownership, or
Phase-18.2 functionality.
