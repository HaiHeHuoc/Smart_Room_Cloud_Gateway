# Smart Room MCP Adapter

`smart_room_mcp_adapter` is the application bridge between Smart Room public
service APIs and provider callbacks registered with `xiaozhi_foundation`. Its
single public lifecycle API, `smart_room_mcp_adapter_register_providers()`, is
called by `smart_room_app` after local services are ready and before production
voice startup.

## Boundaries

- Providers read copied/public state from `sensor_manager`, `cloud_manager`,
  `time_manager`, `sd_card_manager`, `audio_manager`, and `light_manager`.
- Light providers invoke only the public `light_manager` logical-state API;
  they never access GPIO, RMT, NeoPixel, or board mapping.
- `xiaozhi_foundation` remains the sole direct `esp_xiaozhi`/MCP engine and
  session owner. It attaches tools after `esp_mcp_create()` and detaches them
  before `esp_mcp_destroy()`.
- Provider function/context registrations remain borrowed for firmware
  lifetime. This component never retains a managed Xiaozhi handle.

The component only consolidates existing providers. It adds no MCP tool,
protocol, hardware control path, or Phase 18.2 behavior.
