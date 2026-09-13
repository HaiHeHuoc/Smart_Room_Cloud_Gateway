# Xiaozhi Foundation

`xiaozhi_foundation` is the project-owned boundary for the production Xiaozhi
WebSocket session, voice audio channel, bounded semantic text bridge, and
Smart Room MCP registration. Provider handles, credentials, tokens, and
callback-lifetime framework pointers never leave the component.

## Production MCP tools

The following product tools are always compiled and attached to a successfully
started production Xiaozhi session:

- `smart_room.get_current_temperature_humidity`
- `smart_room.get_cloud_sync_status`
- `smart_room.get_system_status`
- `smart_room.light.set_state`
- `smart_room.light.get_state`
- `smart_room.light.get_capabilities`

There is no Menuconfig switch for individual MCP tools. Each tool remains
bounded by its registered provider contract: query tools are read-only, and
the light command validates the complete request before one delegated state
change. Registration does not make tools available while the Xiaozhi session
is stopped or disconnected.

## Lifecycle

`voice_assistant` owns product conversation orchestration and starts the
session after audio manager and both arbiters are ready. `xiaozhi_foundation`
owns the managed provider lifecycle: MCP attaches after engine creation and
detaches before engine destruction. The component does not own I2S, LVGL,
Wi-Fi, cloud, NVS, or GPIO.

## Retired validation infrastructure

The Phase-12 temporary validation runtime, P2-F fixture paths, lifecycle/fault
matrices, resource-attribution tracing, and their Menuconfig settings were
removed during pre-base cleanup. Historical evidence remains in the project
state records; it is not a runnable firmware profile in this source tree.
