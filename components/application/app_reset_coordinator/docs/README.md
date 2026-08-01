# Application Reset Coordinator

## Purpose

`app_reset_coordinator` owns application-level qualification and execution of
the physical Wi-Fi factory-reset transaction. It converts copied `PRESSED`,
`LONG_PRESS`, and `RELEASED` events into at most one request per physical press
cycle, clears both application and ESP-IDF persistent Wi-Fi state, and reboots
only after the cleanup succeeds. It also publishes a copied reset result to
`app_gui` so a verified success receives a bounded display opportunity before
the controlled reboot.

## Ownership

- `button_manager` owns GPIO polling, debounce, and long-press timing.
- `app_reset_coordinator` owns press-cycle ordering, the reset transaction, and
  the final reboot decision.
- `app_network_coordinator` owns the application reset gate, active-operation
  exclusion, and provisioning lifecycle quiescence.
- `config_manager` remains the only owner of persistent configuration writes
  and erasure.
- `wifi_manager` owns Station connection/reconnect behavior and the
  driver-specific `esp_wifi_restore()` cleanup wrapper.
- `provisioning_manager` owns temporary BLE provisioning transport.
- `app_gui` owns reset-result construction, rendering, and presentation
  acknowledgment through its UI task.
- `main` remains the composition root and maps button events into reset input.

The coordinator does not call LVGL, log credentials, or directly call ESP-IDF
Wi-Fi driver APIs.

## Lifecycle

```text
app_reset_coordinator_init()
    -> allocate the four-entry input queue

app_reset_coordinator_start()
    -> create the permanent app_reset task

button callback
    -> app_reset_coordinator_post_input_event()
    -> zero-time copied queue send
    -> return without blocking
```

Initialization and startup are one-shot. There is no stop or deinit API; the
task runs until a verified factory reset restarts the firmware. It uses a
3072-byte stack at priority 4.

## Factory-Reset Transaction

```text
accepted LONG_PRESS
    -> allocate a non-zero boot-local transaction ID
    -> app_network_coordinator_prepare_for_factory_reset(10000)
    -> wifi_manager_clear_persistent_driver_settings()
    -> config_manager_clear_wifi()
    -> verify NOT_CONFIGURED through config_manager
    -> queue SUCCESS through app_gui
    -> wait at most 500 ms for the matching presentation acknowledgment
    -> acknowledged: keep the result visible for 1500 ms
    -> unavailable or unconfirmed GUI: use a bounded 500 ms fallback
    -> esp_restart()
    -> boot policy starts provisioning from clean persistent state
```

Network preparation is always first. It closes the application reset gate,
prevents a new provisioning session or credential handoff, requests one stop
for an active service, and waits at most 10 seconds for manager cleanup and an
already-claimed persistence/adoption transaction. `ESP_OK` leaves that gate
closed until reboot. Only then is driver-owned persistence cleared, followed
by application configuration and `NOT_CONFIGURED` verification. Reboot
discards the old Station, DHCP, reconnect-timer, and coordinator runtime state
instead of trying to enter provisioning from a live connection.

Preparation failure clears no persistent layer and suppresses reboot. A gate
claimed by that failed call is rolled back so release can re-arm a safe retry.
If preparation succeeded but a later idempotent cleanup step fails, the gate
remains closed; a later press may finish cleanup, but normal provisioning
cannot rewrite credentials before reboot.

Each accepted `LONG_PRESS` receives a non-zero transaction ID that is reused
only for that terminal result. The coordinator polls copied GUI state without
calling LVGL. A result is acknowledged only after the UI task rendered the
matching transaction and completed the following `lv_timer_handler()` pass.
GUI queue, construction, or acknowledgment failure never changes the reboot
decision after both persistent cleanup layers have been verified.

A persistent cleanup failure follows a separate path:

```text
preparation/driver/storage/verification failure
    -> queue FAILED through app_gui on a best-effort basis
    -> do not reboot
    -> require RELEASED before another reset attempt
```

## Input Qualification

```text
ARMED
  -- PRESSED --> PRESS_ACTIVE

PRESS_ACTIVE
  -- LONG_PRESS --> REQUEST_ACCEPTED
  -- RELEASED --> ARMED

REQUEST_ACCEPTED
  -- duplicate LONG_PRESS --> ignored
  -- RELEASED --> ARMED
```

An out-of-order `LONG_PRESS` while armed is rejected. Duplicate presses do not
open a second cycle. Release always re-arms the next physical cycle. If the
queue is full, posting fails with `ESP_ERR_TIMEOUT`; the callback never waits.
This failure mode is fail-safe because no reset action is synthesized.

## Public API

- `app_reset_coordinator_init()` allocates the input queue.
- `app_reset_coordinator_start()` creates the qualification task.
- `app_reset_coordinator_post_input_event()` validates and copies one input
  event with zero wait time.

These APIs are task-context only and are not ISR-safe.

## Phase 7.2 Acceptance

Phase 7.2 was manually/hardware accepted by the user on 2026-08-01. The
accepted scope is the non-blocking queue handoff, ordering validation, and
one-shot reset-request qualification.

## Phase 7.3 Acceptance

Phase 7.3 was manually/hardware accepted by the user on 2026-08-01. Acceptance
covered long-press cleanup of both persistent Wi-Fi stores, verified reboot to
`NOT_CONFIGURED`, BLE reprovisioning without `erase-flash`, and successful
IPv4 acquisition after the new connection.

At the Phase 7.3 checkpoint, GUI reset confirmation remained deferred. It is
implemented by Phase 7.4 with build verification complete and hardware
acceptance still pending.

## Phase 7.4 Status

**IMPLEMENTED / BUILD VERIFIED / HARDWARE ACCEPTANCE PENDING**

Phase 7.4 adds copied success/failure results, exact transaction
acknowledgment, the 500 ms acknowledgment budget, the 1500 ms confirmed-success
dwell, and the 500 ms fallback before a verified reboot. Failure remains
non-rebooting and retryable after release.

## Phase 7.5 Status

**IMPLEMENTED / BUILD VERIFIED / HARDWARE ACCEPTANCE PENDING**

Phase 7.5 adds bounded provisioning quiescence before either persistent Wi-Fi
layer is cleared. Reset preparation failure remains non-rebooting and leaves
configuration intact. Hardware tests must still cover every provisioning
lifecycle boundary and a reset racing a verified credential handoff.
