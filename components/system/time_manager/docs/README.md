# `time_manager` Component

`time_manager` owns the application time-service boundary: copied SNTP and
timezone configuration, ESP-NETIF SNTP lifecycle configuration, thread-safe
status snapshots, and access to the ESP-IDF system clock. It does not own
Wi-Fi, GUI/LVGL, cloud/Firebase, NVS, sensors, audio, or an application task.

Default configuration uses Google Public NTP (`time.google.com`) and Vietnam's
POSIX timezone (`ICT-7`). Unix time remains UTC-based; the timezone affects
only `localtime_r()` and ISO-8601 local formatting. No manual UTC+7 arithmetic
or software wall-clock counter is used.

## Network ownership and startup

`main` initializes and configures this component after ESP-NETIF and the
default event loop exist, but before `wifi_manager` can emit its first status
snapshot. `time_manager_start()` configures one ESP-NETIF SNTP instance with
`start = false`, then enters `TIME_MANAGER_STATE_WAITING_NETWORK` without
blocking boot.

The application-owned Wi-Fi fan-out calls:

```c
time_manager_notify_network_state(status->has_ipv4_address);
```

`true` means only that the station has a valid IPv4 address; it does not claim
that Internet reachability or DNS resolution is proven. The first `true` starts
SNTP. A `false` notification returns the lifecycle to `WAITING_NETWORK`; it
does not invalidate a clock that was synchronized earlier.

On a later `true` edge, the component calls the supported
`esp_netif_sntp_start()` restart operation for an early resynchronization.
ESP-IDF v6.0.1 performs the stop/start inside its single ESP-NETIF SNTP service,
so this does not create a second SNTP instance. `time_manager` calls
`esp_netif_sntp_init()` only once per successful component start.

## Status semantics

`state` describes the current network/synchronization lifecycle, while
`synced` describes whether a valid SNTP-synchronized system clock exists.

| Situation | `state` | `synced` |
| --- | --- | --- |
| Boot before valid IPv4 | `WAITING_NETWORK` | `false` |
| IPv4 acquired, waiting for first response | `SYNCING` | `false` |
| SNTP response accepted | `SYNCED` | `true` |
| IPv4 lost after a successful sync | `WAITING_NETWORK` | `true` |

Therefore `time_manager_get_unix_time()`, `time_manager_get_local_time()`, and
`time_manager_format_iso8601()` remain usable while offline after at least one
successful synchronization. Before the first synchronization, those getters
return `ESP_ERR_INVALID_STATE` rather than exposing an unsynchronized 1970-era
clock.

Status and callback registrations are protected by a short critical section.
Callbacks receive a copied snapshot only after that lock is released. SNTP
completion callbacks run from the ESP-IDF TCP/IP task, so application callbacks
must stay short and must not call LVGL directly.

## Retry and periodic resynchronization

The component uses ESP-IDF v6.0.1's supported ESP-NETIF API:

- `esp_netif_sntp_init()` configures the singleton with `wait_for_sync = false`
  and the component synchronization callback.
- `esp_netif_sntp_start()` begins the initial request and safely restarts the
  same service on IPv4 reconnection. It does not wait for DNS or an SNTP reply.

Packet/DNS timeout retry belongs to the underlying lwIP SNTP service. Its
retry/backoff is retained instead of adding a competing task or timer. The
single effective periodic interval is
`CONFIG_LWIP_SNTP_UPDATE_DELAY=1800000` in `sdkconfig.defaults`: 1,800,000 ms
(30 minutes). ESP-IDF v6.0.1 defines the supported range as 15,000 to
4,294,967,295 ms.

There is no custom permanent FreeRTOS time task. Each successful lwIP sync
invokes the ESP-IDF callback, which updates `last_sync_unix`, `sync_count`, and
`last_error` exactly once for that callback.

## GUI consumer

Task 3 uses `time_manager_get_local_time()` from the existing `app_gui` LVGL
timer. The getter returns `ESP_ERR_INVALID_STATE` before the first successful
SNTP synchronization, which lets the GUI show an explicit placeholder. Once
the manager has synchronized, the getter reads the system clock and remains
usable while Wi-Fi is offline; the manager has no GUI/LVGL dependency.

## Deferred work

Task 4 will add Firebase time telemetry and its later publish-period decision.
NVS time persistence, external RTC support, REST time APIs, and long-duration
hardware validation are intentionally outside this component's current scope.
