# app_log

## Purpose

`app_log` is the lightweight application logging frontend. It gives producers a
consistent event-oriented interface while leaving persistent storage, file
rotation, and retention to `log_manager`.

## Public Producer API

- `APP_LOGE`, `APP_LOGW`, `APP_LOGI`, `APP_LOGD`, and `APP_LOGV` lazily evaluate
  their formatting arguments only when an active sink accepts the requested
  level.
- `app_log_should_emit()` checks the currently registered filter.
- `app_log_emit()` sends one formatted event to the registered sink.

Tags and event tokens should be short static identifiers. Details should be
bounded `key=value` data and must never include passwords, tokens, activation
codes, private URLs, or raw private payloads.

## Backend Boundary

`app_log_register_backend()` is composition/backend-only and task-context only.
The backend function and filter must remain valid for firmware lifetime. The
frontend does not open files, allocate per record, own SD/time state, or impose
storage policy. `log_manager` is the current persistent backend owner.

## Context And Limitations

The API is for normal task context. It is not ISR-safe, cache-disabled-safe, or
panic-safe. Console output can block according to ESP-IDF logging behavior;
persistent logging must not wait on a producer-held ring mutex. A missing
backend simply means no persistent sink is selected.

## Validation Boundary

The component is a formatting/dispatch boundary, not a promise that an event
has reached SD storage. Persistent durability, filtering, retention, and media
failure behavior are documented by [`log_manager`](../log_manager/README.md).
