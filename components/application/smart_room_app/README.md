# Smart Room Product Composition

`smart_room_app` is the product-level composition component called once from
the thin `main/main.c` ESP-IDF entrypoint. It preserves the established
Gateway startup order, application-owned policy values, and bounded
cross-component callback routing.

## Responsibilities

- Initialize platform services, display/UI, storage recovery, input/reset,
  Wi-Fi/network coordination, cloud, sensing, light, and deferred audio in the
  existing order.
- Keep board mapping in `common/board_config.h`; inject that mapping into
  manager configuration without taking GPIO, RMT, I2S, LVGL, or driver
  ownership.
- Keep product policy here: sensor cadence/staleness, provisioning timing,
  cloud publish cadence and deployment endpoint, Firebase-auth references, and
  audio/network startup gating.
- Copy manager snapshots into GUI/cloud/coordinator inputs. Callbacks do not
  call LVGL or execute long network/storage work.
- Register the bounded Smart Room MCP provider set through
  `smart_room_mcp_adapter` before a production voice session can start.

## Configuration Classification

| Value class | Owner/location | Rule |
|---|---|---|
| Physical board mapping | `common/board_config.h` | Single hardware source of truth; do not duplicate pins here. |
| Product/application policy | `smart_room_app.c` | Compose existing public manager configurations. |
| Component-local default | Owning component | Keep with the component unless composition must select a product policy. |
| Deployment identifier | Product cloud configuration | URL/device path is not a credential; do not log it from MCP. |
| Development secret | Generated local configuration via `app_common.h` macros | Never add literal credentials or stage local `sdkconfig`. |

## Non-ownership

`smart_room_app` does not replace `audio_manager`, `app_gui`,
`xiaozhi_foundation`, `light_manager`, `cloud_manager`, or any driver. It is a
product-specific composition component, not a generic service facade.

The Phase-16 target-HIL coordinator was retired during pre-base cleanup. The
optional audio public-API stress task, when explicitly enabled in the audio
manager menu, starts directly from this composition root. Phase 18.2 remains
not started.
