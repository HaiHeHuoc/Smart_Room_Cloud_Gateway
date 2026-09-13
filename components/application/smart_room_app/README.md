# Smart Room Product Composition

`smart_room_app` is the product-level composition component called once from
the thin `main/main.c` ESP-IDF entrypoint. It owns the established Gateway
startup order, application-owned policy values, and bounded cross-component
callback routing.

## Responsibilities

- Initialize platform services, display/UI, storage recovery, input/reset,
  Wi-Fi/network coordination, cloud, sensing, light, and deferred audio/voice
  in the established dependency order.
- Keep physical board mapping in `common/board_config.h`; inject that mapping
  into manager configuration without taking GPIO, RMT, I2S, LVGL, or driver
  ownership.
- Keep product policy here: sensor cadence/staleness, provisioning timing,
  cloud publish cadence/deployment endpoint, Firebase-auth references, and
  audio/network startup gating.
- Copy manager snapshots into GUI/cloud/coordinator inputs. Callbacks must not
  call LVGL or execute long network/storage work.
- Register the bounded Smart Room provider set through
  `smart_room_mcp_adapter` before production voice startup.

## Configuration Classification

| Value class | Owner/location | Rule |
|---|---|---|
| Physical board mapping | `common/board_config.h` | Single hardware source of truth; do not duplicate pins here. |
| Product/application policy | `smart_room_app.c` | Compose existing public manager configurations. |
| Component-local default | Owning component | Keep with the component unless composition must select a product policy. |
| Deployment identifier | Product cloud configuration | URL/device path is not a credential; do not expose it unnecessarily through MCP. |
| Development secret | Generated local configuration via `app_common.h` macros | Never add literal credentials or tracked local `sdkconfig`. |

## MCP Composition Boundary

```text
smart_room_app
    -> smart_room_mcp_adapter_register_providers()
        -> xiaozhi_foundation provider registration
            -> production MCP tools attached by xiaozhi_foundation session
```

`smart_room_app` does not register every individual domain provider itself.
`smart_room_mcp_adapter` owns the provider adaptation layer, while
`xiaozhi_foundation` owns the managed MCP engine/session and tool attachment.

## Audio / Voice Startup Policy

`smart_room_app` defers audio/voice startup until the application network
coordinator reaches the required online/handoff state. This is product lifecycle
policy only; it does not transfer I2S/DMA ownership out of `audio_manager`.

Normal `audio_manager` startup reaches production `IDLE` and waits for explicit
operations. The former Phase-16 target-HIL coordinator, legacy direct-I2S
`audio_test`, and audio public-API stress configuration/support were retired
during application-structure cleanup and are not selectable current product
profiles.

## Non-ownership

`smart_room_app` does not replace or directly own internals of:

- `audio_manager`;
- `app_gui` / `ui_manager_lvgl`;
- `xiaozhi_foundation`;
- `light_manager` / NeoPixel driver;
- `cloud_manager` / `firebase_auth`;
- Wi-Fi/provisioning managers;
- storage or device drivers.

It is a product-specific composition component, not a generic service facade.

## Roadmap Guard

The structure cleanup adds no MCP tool and does not start Phase 18.2. Current
roadmap state remains Phase 18 in progress with Phase 18.1 complete and
18.2-18.4 not started until explicitly requested.
