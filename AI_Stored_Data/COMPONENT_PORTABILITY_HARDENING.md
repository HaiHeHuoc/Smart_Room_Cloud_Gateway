# Component Portability Hardening

## Scope

Working branch: `refactor/component-portability-hardening`

Base branch/commit at refactor start:

- `main_including_Firebase_security`
- `52721668e9a7682db1b9e42d9971a7d322c3a420`

Goal: improve practical ESP-IDF component reuse, dependency direction, and
large-component internal organization without changing product architecture or
roadmap ownership.

This refactor intentionally targets ESP-IDF/ESP32 reuse. It does not claim
cross-platform STM32, Zephyr, or Linux portability.

## Architecture decisions

### 1. Three reuse levels

1. **Reusable component library** — should be easy to copy into another ESP-IDF
   project with standard dependency/configuration work.
2. **Platform/service component** — reusable on the same platform after board or
   provider configuration.
3. **Product/application component** — Smart-Room-specific orchestration; clean
   boundaries matter more than generic reuse.

Application coordinators and product GUI are not to be genericized merely to
raise a portability score.

### 2. Dependency direction

Preferred direction:

```text
Application -> Service -> Driver / ESP-IDF
```

Reusable lower-level components must not depend on application coordinators or
`app_gui`.

CMake visibility rule:

- a dependency whose type/header appears in a public component header belongs in
  `REQUIRES`;
- implementation-only dependencies belong in `PRIV_REQUIRES`.

### 3. Large component private modules

A tightly coupled child subsystem with no independent reuse/lifecycle value is a
private module, not a new ESP-IDF component.

Preferred form:

```text
parent_component/
|-- CMakeLists.txt
|-- include/                  # public facade API
|-- parent_component.c        # parent lifecycle/facade
`-- modules/
    `-- child_name/
        |-- include/          # private headers when needed
        |-- src/
        `-- README.md
```

Rules:

- parent CMake owns all child sources;
- no child `CMakeLists.txt` by default;
- other components never include another component's `modules/` headers;
- each private module has a semantic README;
- promote a child to a standalone component only after a real independent reuse
  or lifecycle requirement appears.

### 4. Centralized project configuration

`board_config.h` remains the single Smart Room source of truth for physical
hardware mapping. Do not move every board constant into a reusable component
just for theoretical independence.

`app_common.h` remains application identity/configuration rather than a generic
constant bucket.

Equal numeric values in different APIs are not sufficient reason to centralize
those constants.

## Structural refactor completed

Private modules now organize the large components:

- `audio_manager/modules/`
  - `arbitration`
  - `playback`
  - `stream`
  - `dsp`
  - `wav`
  - `test_support`
- `log_manager/modules/`
  - `buffer`
  - `console`
- `cloud_manager/modules/telemetry_json`
- `voice_assistant/modules/`
  - `audio`
  - `ui`
  - `ptt`
  - `codec`
  - `uplink`
  - `downlink`
- `xiaozhi_foundation/modules/`
  - `session`
  - `text_bridge`
  - `websocket`
  - `fixture`
- UI private modules:
  - `app_gui/modules/theme`
  - `lvgl_image_handler/modules/animated_gif`
  - `ui_manager_lvgl/modules/allocator`

The moved implementation files were preserved as Git renames; the structural
moves themselves do not intentionally change runtime behavior.

Host-test paths were updated for moved audio, cloud telemetry, and logging
sources. A stale `log_manager` host-test path was found during validation and
fixed.

## Dependency hardening completed

Public/private CMake dependency surfaces were narrowed for the affected
components. Important examples:

- `audio_manager`: facade API does not expose I2S/GPIO/SD/common/logging internals;
- `log_manager`: public API exposes ESP error/log-level types while persistent
  backend providers remain private;
- `cloud_manager`: telemetry/provider implementation dependencies are private;
- `lvgl_sd_fs` and `lvgl_image_handler`: removed stale `display_driver`
  dependency where source/public API did not use it;
- `app_gui`: LVGL/runtime dependencies are private because its public model API
  does not expose LVGL types;
- application coordinators: implementation services remain private;
- `display_driver`: ESP LCD handle types remain public, board/GPIO/SPI/vendor
  integration remains private.

`app_log` intentionally remains independent from `log_manager`; ordinary logging
producers do not need an SD/time backend dependency.

## Leaf-component changes

### DHT22

`sensor_DHT22` now receives its GPIO through `dht22_sensor_config_t` rather than
reading `board_config.h` directly.

`sensor_manager` maps the Smart Room board pin into this leaf configuration.
The leaf validates that the configured GPIO is output-capable before accepting
initialization.

This makes the DHT22 wrapper materially easier to reuse without moving product
sampling/staleness policy into the driver.

### SD path-size ownership

`SD_CARD_MANAGER_PATH_MAX_LEN` moved from `board_config.h` into
`sd_card_manager.h` because it is a storage/API contract, not physical board
mapping.

### Button task stack policy

`button_manager` now uses ESP-IDF's explicit PSRAM task-creation API rather than
depending on the project `common` helper.

`sdkconfig.defaults` now enables:

```text
CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y
```

This is required by ESP-IDF for explicit task stacks allocated in external RAM.

## Configuration/security cleanup

Firebase development values are no longer hard-coded in `app_common.h` on the
refactor branch. The header maps to `CONFIG_APP_FIREBASE_*` options defined in
`main/Kconfig.projbuild`; defaults are empty and real development values belong
only in local generated configuration.

Never reintroduce real account credentials into tracked source or documentation.

## Intentionally not over-refactored

The following are deliberate boundaries, not unfinished mistakes:

- `audio_manager` remains a platform/service component and still consumes the
  centralized audio board mapping; converting its large lifecycle to a runtime
  pin config was not justified without a build/HIL gate.
- `sd_card_manager` remains board-integrated for SDSPI host/pins/mount policy in
  this wave. Its dependency visibility was cleaned without rewriting the
  recovery state machine.
- `display_driver` and `ui_manager_lvgl` are current-board/platform integration
  components; use of centralized display configuration is allowed.
- `wifi_manager` still uses the component-scoped `psram_task_create.h` compile
  override from `common`. Its reconnect worker ownership was inspected, but a
  large source rewrite only to remove this helper was judged higher risk than
  the portability gain in this wave.
- `app_network_coordinator`, `app_reset_coordinator`, `app_gui`,
  `voice_assistant`, and application cloud schema/policy are intentionally
  product-specific.
- Xiaozhi's validation-only SD fixture keeps its existing conditional test
  integration. Do not redesign fixture ownership without a separate decision.

## Validation performed in this refactor session

Static validation completed:

- branch remains a clean descendant of the selected base;
- moved implementation files are represented as Git renames rather than
  behavior rewrites;
- CMake source paths and private include paths were reviewed for the moved
  modules;
- source-file compile-definition paths were updated for moved voice/Xiaozhi
  files;
- host-test source/include paths were updated for audio/cloud/logging;
- public-header dependency visibility was reviewed for the components changed in
  this wave;
- ESP-IDF documentation was checked for `REQUIRES`/`PRIV_REQUIRES` semantics and
  explicit external-RAM task-stack prerequisites;
- centralized configuration ownership was reviewed after the refactor.

The current ChatGPT execution environment could not resolve GitHub from the
local shell, so it could not clone the branch or execute `idf.py build` itself.
No full firmware build or target-board acceptance is claimed by this file.

## Required acceptance gate on a machine with ESP-IDF 6.0.1

Run from a clean checkout of `refactor/component-portability-hardening`:

```powershell
idf.py fullclean
idf.py build

& components/audio/audio_manager/test/host/run_tests.ps1
& components/cloud/cloud_manager/test/host/run_tests.ps1
& components/system/log_manager/test/host/run_tests.ps1
```

Then flash the existing Smart Room hardware image and perform a bounded smoke
check of at least:

1. boot and GUI initialization;
2. Wi-Fi connect/reconnect;
3. SD mount/recovery and persistent logging;
4. DHT22 sensor sampling;
5. LVGL SD image path;
6. audio manager capture/playback path used by the current branch;
7. voice/Xiaozhi path relevant to the current Phase-16.1 baseline.

Any failure after the structural moves must be treated as a refactor regression
until proven otherwise.

## Refactor completion definition

The implementation and repository documentation for this portability-hardening
wave are complete when:

- no additional architecture/ownership refactor is required for the agreed
  scope;
- all private module structure and CMake references are consistent;
- the clean ESP-IDF build and host regression tests pass;
- the bounded hardware smoke test shows no behavior regression.

Do not merge into `main_including_Firebase_security` automatically. Create/review
a PR only when Hải explicitly requests it; Hải owns the final merge decision.
