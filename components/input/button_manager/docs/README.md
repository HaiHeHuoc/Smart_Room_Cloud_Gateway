# Button Manager

## Purpose

`button_manager` owns one polled GPIO button and publishes debounced press,
release, and one-shot long-press events. Phase 7.1 provides input detection
only. It does not erase configuration, restart provisioning, reboot, or call
LVGL.

## Ownership

- `button_manager` owns GPIO input configuration, polling, debounce timing,
  hold timing, its task, and callback publication.
- `main` owns component composition and maps copied events to application work.
- Future factory-reset orchestration must call `config_manager` and network/UI
  owners from an application task, never from the button callback.
- The callback must not perform blocking storage/network work or call LVGL.

## Configuration

The production configuration is defined in `board_config.h`:

| Setting | Current value |
|---|---:|
| GPIO | 9 |
| Active level | 0 (active-low) |
| Poll period | 10 ms |
| Debounce | 40 ms |
| Long press | 5000 ms |

Active-low input enables the internal pull-up; active-high input enables the
internal pull-down. Hardware wiring must match this policy.

## Public API

- `button_manager_init()` validates and copies configuration, then configures
  the GPIO. It does not create a task.
- `button_manager_register_callback()` stores one callback/context pair after
  initialization and before start.
- `button_manager_start()` creates the permanent polling task and returns.

Phase 7.1 intentionally has no stop, deinit, callback replacement, or status
snapshot API.

## Event Flow

```text
GPIO sample every poll period
    -> candidate level changes
    -> candidate remains stable for debounce_ms
    -> publish PRESSED or RELEASED once
    -> while stably pressed, publish LONG_PRESS once at long_press_ms
    -> main callback logs the copied event and returns
```

`held_ms` is zero for `PRESSED`. For `RELEASED`, it is measured from the
debounced press to the beginning of the stable release candidate. For
`LONG_PRESS`, it is measured from the debounced press.

## Task And Callback Contract

- Task name: `button_manager`
- Stack: 3072 bytes on ESP-IDF, where `xTaskCreate()` stack depth is bytes
- Priority: 4
- Lifecycle: created once and runs for the firmware lifetime
- Wait behavior: finite periodic delay using `vTaskDelayUntil()`
- Callback context: button polling task, never ISR context
- Callback event pointer: temporary and valid only until callback return

No mutex is required in Phase 7.1 because configuration and callback
registration finish before the task starts and are immutable while running.

## Failure Behavior

- Invalid GPIO or timing returns `ESP_ERR_INVALID_ARG`.
- Repeated or out-of-order lifecycle calls return `ESP_ERR_INVALID_STATE`.
- GPIO driver errors are returned from initialization.
- Task allocation failure returns `ESP_ERR_NO_MEM` and restores the initialized
  lifecycle so start can be retried.
- Failure disables only button input; `main` continues starting other services.

## Phase 7.1 Acceptance

Phase 7.1 was manually/hardware accepted by the user on 2026-08-01. The
accepted checkpoint covers GPIO 9 active-low polling, 40 ms debounce, single
press/release events, one long-press event after approximately 5 seconds, and
continued startup of unrelated services.

Factory-reset execution, NVS clearing, provisioning restart, and LVGL reset
confirmation remain deferred to later Sprint 7 checkpoints.
