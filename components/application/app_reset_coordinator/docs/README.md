# Application Reset Coordinator

## Purpose

`app_reset_coordinator` owns application-level qualification of physical
factory-reset input. Phase 7.2 converts copied `PRESSED`, `LONG_PRESS`, and
`RELEASED` events into at most one accepted reset request per physical press
cycle.

The accepted request remains diagnostic-only in this checkpoint. No Wi-Fi
configuration is erased and no provisioning, GUI, or reboot action is taken.

## Ownership

- `button_manager` owns GPIO polling, debounce, and long-press timing.
- `app_reset_coordinator` owns press-cycle ordering and one-shot request
  qualification.
- `config_manager` remains the only owner of persistent configuration writes
  and erasure.
- `wifi_manager` owns Station connection and reconnect behavior.
- `provisioning_manager` owns temporary BLE provisioning transport.
- `app_gui` owns screen changes through its UI task.
- `main` remains the composition root and maps button events into reset input.

The coordinator does not call LVGL and does not access credentials.

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

Initialization and startup are one-shot. Phase 7.2 provides no stop, deinit,
or restart operation. The task uses a 3072-byte stack at priority 4.

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

The following remain deferred:

- clearing Wi-Fi configuration through `config_manager`;
- coordinating Wi-Fi/provisioning lifecycle changes;
- GUI reset confirmation;
- reboot or same-boot recovery policy;
- end-to-end recovery testing.
