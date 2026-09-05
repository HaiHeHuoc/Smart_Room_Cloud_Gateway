# BOOT `Starting...` Debug Handoff

Updated: 2026-08-25
Branch: `phase/12.5-transport-validation-clean`
Status: **CODE MITIGATION IMPLEMENTED / BUILD + HIL PENDING**

## Symptom reported

Observed on target previously: LCD could remain on the built-in BOOT screen showing:

```text
Smart Gateway
Starting...
```

No hardware is currently available to reproduce or validate the issue.

## Important clarification

There are two visually similar startup states in the current UI:

1. BOOT screen: literal text `Starting...` from `app_gui_render_boot_status()`.
2. Provisioning UI: `UI_PROVISIONING_STATE_STARTING` rendered as `Starting setup...`.

The current mitigation specifically targets an **indefinite BOOT `Starting...` screen**, not the provisioning state machine.

## Verified causal holes in current code

The normal stored-Wi-Fi path already has a bounded BOOT grace:

- coordinator state `CONNECTING` or `OFFLINE`;
- wait up to 60 seconds;
- if still on BOOT and not ONLINE, request `APP_GUI_SCREEN_WIFI_STATUS`.

Source review found three classes of uncovered failure paths that could leave BOOT visible after startup had already failed.

### A. `app_network_coordinator_start()` fails

`app_main()` logged the error and returned while BOOT was already active. No fallback screen was requested.

### B. Coordinator task starts but boot policy fails

`app_network_coordinator_task()` moves its state to `APP_NETWORK_COORDINATOR_STATE_FAILED` and exits. The existing 60-second BOOT grace is only executed on the successful boot-policy return path, so a failure can leave the already active BOOT screen visible indefinitely.

### C. Fatal manager initialization fails after BOOT but before coordinator start

BOOT is rendered before Wi-Fi/coordinator/cloud/sensor composition is completed. The following fatal failures previously logged and returned directly from `app_main()` while BOOT remained active:

- `wifi_manager_init()`;
- `wifi_manager_register_status_callback()`;
- `app_network_coordinator_init()`;
- `firebase_auth_init()`;
- `cloud_manager_init()`;
- `cloud_manager_register_status_callback()`;
- `sensor_manager_init()`;
- `sensor_manager_register_callback()`;
- `sensor_manager_start()`.

These are real UI/lifecycle holes supported by source inspection. They do **not** prove which one caused the previously observed hardware symptom.

## Implemented mitigation

### Mitigation 1

Commit:

`d6bb20de9430c988d3658187bad5fdd3835aa787` — `fix(startup): leave boot screen on network failure`

Behavior added:

1. If `app_network_coordinator_start()` fails after BOOT is active, `app_main()` best-effort requests `APP_GUI_SCREEN_WIFI_STATUS` before returning.
2. During deferred startup polling, if coordinator state becomes `APP_NETWORK_COORDINATOR_STATE_FAILED` while the active screen is still `APP_GUI_SCREEN_BOOT`, `app_main()` best-effort requests `APP_GUI_SCREEN_WIFI_STATUS`.
3. The fallback is one-shot after a successful queue request.
4. If the active screen is already provisioning, reset-result, Wi-Fi status, sensor dashboard, Xiaozhi, or another explicit route, this guard does not override it.

### Mitigation 2

Commit:

`7c63d34ae795632657bc227b294eeeba1bc3bb84` — `fix(startup): route fatal boot exits through fail-safe`

Behavior added:

1. Adds one composition-layer helper:

   `app_route_boot_failure_to_wifi_status(stage, error)`

2. The helper first reads the active screen.
3. It requests `APP_GUI_SCREEN_WIFI_STATUS` only when the current screen is still `APP_GUI_SCREEN_BOOT`.
4. If provisioning/reset/another explicit route has already replaced BOOT, it preserves that route and only logs the startup failure.
5. It records a non-sensitive `stage` string and `esp_err_t` name so later serial traces identify the exact fatal startup boundary.
6. All fatal manager-init exits listed in class C now call the helper before returning.
7. The earlier coordinator-start fallback was consolidated into the same helper.
8. No Wi-Fi ownership, provisioning state, Xiaozhi transport, cloud lifecycle, audio lifecycle, NVS data, or reset state machine is modified.

This establishes the intended composition invariant:

> After BOOT has been rendered, a fatal startup return must best-effort leave BOOT before `app_main()` exits, unless another explicit application screen has already taken ownership of the UI.

The mitigation is intentionally a **composition-layer fail-safe**, not a guessed repair to `wifi_manager`, `provisioning_manager`, Xiaozhi, cloud, or sensor internals.

## Why Xiaozhi is not currently the primary direct suspect

Current startup ordering requires Xiaozhi validation to wait for:

- cloud manager started;
- audio manager started;
- network coordinator ONLINE;
- audio IDLE / no active capture or playback;
- supported cloud steady state;
- Xiaozhi validation screen active for the configured quiescence interval.

Therefore a BOOT-stage failure occurs before the Xiaozhi transport worker should run.

Xiaozhi may still change build/resource/timing characteristics and expose a latent issue; this remains a hypothesis to test, not a confirmed cause.

## Recent suspect commits reviewed

### `4f33b207913ba1c47069b833b9f7fd7e0c6b13ab`

`test(xiaozhi): isolate lifecycle resource attribution [12.6]`

Relevant changes:

- moved automatic Xiaozhi validation orchestration out of `app_network_coordinator` into `main` steady-state composition;
- removed `xiaozhi_foundation` dependency and direct Xiaozhi trigger from network coordinator;
- added deferred cloud/audio/Xiaozhi readiness ordering in `main.c`;
- changed startup timing/composition, but did not modify the provisioning `STARTING -> provisioning_manager_start() -> WAITING_FOR_PHONE` transition.

Assessment: possible timing/resource regression boundary, but low evidence for a direct BOOT/provisioning logic regression.

### `53481f66060af839472a1871c8e244f82bc4669a`

`refactor: gate Xiaozhi Phase 12 validation runtime`

Relevant changes:

- introduced `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE`, default OFF;
- gated Xiaozhi include/calls/routes in network coordinator;
- retained the then-existing Xiaozhi trigger after ONLINE/provisioning success only when enabled;
- did not modify the core provisioning startup transition.

Assessment: lower direct suspicion than `4f33b207`.

## Hardware validation plan for Codex

When hardware becomes available, do not immediately rewrite the mitigation. First reproduce and collect evidence.

### Test 1 — normal stored Wi-Fi boot

Expected:

```text
BOOT
-> network coordinator starts
-> stored Wi-Fi CONNECTING / WAITING_FOR_IP
-> ONLINE
-> normal post-network UI
```

Confirm no startup fail-safe marker appears.

### Test 2 — stored Wi-Fi unavailable

Turn off the configured AP before boot.

Expected existing policy:

- BOOT remains for bounded stored-Wi-Fi grace;
- approximately 60 seconds later system routes to `WIFI_STATUS`;
- reconnect ownership remains in `wifi_manager`;
- the new fatal-startup helper should not activate because this is a recoverable connectivity condition, not a fatal composition failure.

### Test 3 — force/observe coordinator FAILED while BOOT is active

Do not add unsafe private fault injection. Use a legitimate reproducible public/configuration failure if one is available.

Expected mitigation marker:

```text
Network coordinator entered FAILED while BOOT was active; WIFI_STATUS queued to prevent an indefinite Starting screen
```

LCD should leave BOOT and show `WIFI_STATUS`.

### Test 4 — fatal pre-coordinator startup boundary

If a safe test configuration can make one of the fatal initialization APIs return an error after BOOT is visible, expect a marker in this form:

```text
Startup failure stage=<stage> error=<esp_err_name>; WIFI_STATUS queued to prevent an indefinite BOOT Starting screen
```

Useful stage values now include:

- `wifi_manager_init`;
- `wifi_manager_register_status_callback`;
- `app_network_coordinator_init`;
- `firebase_auth_init`;
- `cloud_manager_init`;
- `cloud_manager_register_status_callback`;
- `sensor_manager_init`;
- `sensor_manager_register_callback`;
- `sensor_manager_start`;
- `app_network_coordinator_start`.

### Test 5 — provisioning regression

Erase Wi-Fi configuration through the supported project path and reboot.

Expected:

```text
BOOT
-> configuration resolves NOT_CONFIGURED
-> PROVISIONING screen
-> STARTING
-> WAITING_FOR_PHONE
```

The BOOT fail-safe must never override the provisioning screen.

### Test 6 — factory-reset route

Exercise the supported reset path while startup/provisioning is active.

Expected:

- reset-result/reset-owned screen remains authoritative;
- the BOOT fail-safe does not overwrite it with `WIFI_STATUS`.

### Test 7 — Xiaozhi feature gate comparison

Run both:

- `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=n`
- explicitly enabled validation image

Compare boot/provisioning behavior and serial ordering. Xiaozhi worker must not start before ONLINE/audio/cloud prerequisites.

### Regression boundary if symptom persists

Compare the same NVS/config state and hardware conditions at:

- parent `33e11fa31d621271bef51c1d1ad1e8ffebde1e03`
- suspect `4f33b207913ba1c47069b833b9f7fd7e0c6b13ab`

If the parent is stable and `4f33b207` reproduces the symptom, inspect startup timing/resource differences introduced by that commit before changing provisioning logic.

## Logs to preserve

Capture from reset until at least the first stable UI/network state. Keep especially:

- `MAIN_APP`
- `APP_NETWORK_COORDINATOR`
- `WIFI_MANAGER`
- `PROVISIONING_MANAGER`
- `APP_GUI`
- `CLOUD_MANAGER`
- `AUDIO_MANAGER`
- `XIAOZHI_FOUNDATION` when validation is enabled

Important checkpoints:

- `Network coordinator task started`
- resolved Wi-Fi configuration state
- initial screen route
- `Network boot policy started successfully`
- `Network coordinator task failed: ...`
- `Waiting up to 60000 ms for stored Wi-Fi before leaving boot screen`
- `Stored Wi-Fi is still unavailable; leaving boot screen`
- `Startup failure stage=... error=...`
- coordinator FAILED BOOT fallback marker

## Acceptance before declaring fixed

Do **not** mark this bug fixed solely because the LCD no longer stays on BOOT.

Acceptance requires hardware evidence that:

- normal stored-Wi-Fi boot still works;
- AP-unavailable boot exits BOOT after the existing bounded grace;
- coordinator failure exits BOOT instead of remaining indefinitely;
- fatal manager-init failures after BOOT route away from stale BOOT without screen-queue spam;
- provisioning path is not overridden;
- factory-reset route is not overridden;
- no watchdog, panic, assert, or repeated screen-queue spam appears;
- feature-off Xiaozhi normal gateway behavior remains intact.

Until then classify this item as:

**Mitigation implemented; root cause and hardware acceptance pending.**
