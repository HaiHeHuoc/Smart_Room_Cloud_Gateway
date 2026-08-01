# Application Reset Coordinator

## Purpose

`app_reset_coordinator` owns application-level qualification and execution of
the physical Wi-Fi factory-reset transaction. It converts copied `PRESSED`,
`LONG_PRESS`, and `RELEASED` events into at most one request per physical press
cycle, clears both application and ESP-IDF persistent Wi-Fi state, and reboots
only after the cleanup succeeds.

## Ownership

- `button_manager` owns GPIO polling, debounce, and long-press timing.
- `app_reset_coordinator` owns press-cycle ordering, the reset transaction, and
  the final reboot decision.
- `config_manager` remains the only owner of persistent configuration writes
  and erasure.
- `wifi_manager` owns Station connection/reconnect behavior and the
  driver-specific `esp_wifi_restore()` cleanup wrapper.
- `provisioning_manager` owns temporary BLE provisioning transport.
- `app_gui` owns screen changes through its UI task.
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
    -> wifi_manager_clear_persistent_driver_settings()
    -> config_manager_clear_wifi()
    -> verify NOT_CONFIGURED through config_manager
    -> wait 500 ms for the terminal serial diagnostic
    -> esp_restart()
    -> boot policy starts provisioning from clean persistent state
```

Driver-owned persistence is cleared first. If Wi-Fi is not initialized yet or
driver cleanup fails, the authoritative application configuration remains
intact. Application configuration is then cleared and verified. A failure at
either layer suppresses reboot and is safe to retry because both cleanup
operations are idempotent. Reboot discards the old Station, DHCP,
reconnect-timer, and coordinator runtime state instead of trying to enter
provisioning from a live connection.

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

The following remains deferred:

- GUI reset confirmation;

## Known Limitation

Phase 7.3 acceptance covers reset from the normal configured-device runtime.
Reset is not yet coordinated against an active provisioning credential
handoff; provisioning could rewrite Wi-Fi state while cleanup is running. A
later checkpoint must quiesce or reject reset during that lifecycle before the
gesture is considered safe in every network state.
