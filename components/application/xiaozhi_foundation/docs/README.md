# Xiaozhi Foundation — Phases 12.3–12.4.3

## Goal

Phase 12.3 validates the Xiaozhi service-information and activation path without
starting the chat/audio lifecycle. The project keeps the external component
behind the `xiaozhi_foundation` boundary and exposes only copied,
non-sensitive scalar state.

## Runtime Flow

```text
Wi-Fi ONLINE
    -> app_network_coordinator
    -> xiaozhi_foundation_request_probe()
    -> private worker task
    -> esp_xiaozhi_chat_get_info()
    -> copy non-sensitive flags
    -> esp_xiaozhi_chat_free_info()
```

`xiaozhi_foundation_request_probe()` is intentionally non-blocking because the
underlying information request performs HTTP/TLS work and can wait up to the
component-configured timeout. Wi-Fi callbacks and coordinator event paths only
schedule the worker and do not execute the HTTP request directly.

## Endpoint And Device Identity

For `esp_xiaozhi` 0.1.2 the information request uses the component Kconfig
endpoint `CONFIG_XIAOZHI_OTA_URL`. The managed component builds a board identity
before the request:

- `Client-Id`: a generated UUID persisted by Xiaozhi in the private NVS
  namespace `board` under key `uuid`.
- `Device-Id`: the ESP base MAC address.
- `User-Agent`: ESP-IDF target name.

The UUID is generated only when not already present, so normal reboot preserves
Xiaozhi device identity and account binding.

## Activation Flow

The server response may contain `activation.code`, `activation.challenge`,
`activation.message`, and `activation.timeout_ms`. The project does not expose
or persist those returned string pointers through its public API.

Observed target-hardware sequence for Phase 12.3:

1. Initial probe reached the Xiaozhi service and returned both MQTT and
   WebSocket transport configuration.
2. Before account binding, activation code and challenge were present.
3. The device was bound to the user's Xiaozhi account/agent through the Xiaozhi
   console.
4. After reboot/binding, the probe again reached the service, MQTT and
   WebSocket remained available, and both activation code and challenge were
   absent.
5. `activation_timeout_ms` was `0` in the observed response. In the managed
   component this field is reset to zero before parsing and changes only when
   the server supplies a numeric timeout, so zero is treated as "no useful
   server-provided activation timeout" rather than a local failure.

## Ownership And Cleanup

`esp_xiaozhi_chat_get_info()` can allocate string fields in
`esp_xiaozhi_chat_info_t`. `xiaozhi_foundation_probe()` copies only booleans and
an integer into caller-owned storage, then calls `esp_xiaozhi_chat_free_info()`.
On a failed `get_info()` it still calls `free_info()` because partial parsing may
have allocated fields.

The public `xiaozhi_foundation_info_t` never contains:

- activation code or challenge strings;
- serial number or firmware URL strings;
- MQTT/WebSocket endpoints, tokens, usernames, passwords, or topics;
- any Xiaozhi-owned pointer or handle.

## Private Xiaozhi Storage Audit

Phase 12.3 observed/audited these private namespaces owned by the managed
Xiaozhi component:

| Namespace | Purpose | Sensitivity | Project policy |
|---|---|---|---|
| `board` | persistent Xiaozhi UUID/device identity | identity | preserve |
| `mqtt` | server-provided MQTT endpoint/credentials/topics | secret | preserve/private |
| `websocket` | server-provided WebSocket URL/token/version | secret | preserve/private |

`esp_xiaozhi_chat_get_info()` persists MQTT and WebSocket response objects into
those namespaces when present. The project does not read their credentials and
does not mirror them into `config_manager`.

## Reset Policy

The current project long-press reset is a **Wi-Fi configuration reset**, not a
full device identity wipe. It clears project Wi-Fi credentials and Wi-Fi driver
persistence while preserving other device identity/configuration.

Phase 12.3 therefore defines the Xiaozhi reset policy as:

- preserve `board`, `mqtt`, and `websocket` during the existing Wi-Fi reset;
- do not erase Xiaozhi namespaces from `app_reset_coordinator`;
- preserve the bound Xiaozhi UUID across Wi-Fi reprovisioning and normal reboot;
- if a future explicit "full device identity reset" is introduced, Xiaozhi
  unbind/credential erasure must be designed and verified separately before
  these namespaces are removed.

This prevents a Wi-Fi reset from silently generating a new Xiaozhi identity or
breaking account binding.

## Timeout And Error Recovery

The information request uses `CONFIG_XIAOZHI_INFO_TIMEOUT_MS` from the managed
component (30 seconds by default in 0.1.2). `xiaozhi_foundation` does not add an
unbounded retry loop.

Current recovery policy is event-driven and bounded by the existing Wi-Fi
lifecycle:

- one probe is scheduled when application network state enters `ONLINE`;
- only one probe may run at a time; overlapping requests return
  `ESP_ERR_INVALID_STATE` and are ignored as an already-running probe;
- HTTP/TLS/parse/NVS failures are logged only by error code/name, then the
  worker exits and releases the in-progress gate;
- an `OFFLINE -> ONLINE` recovery transition schedules a fresh probe;
- Phase 12.3 intentionally does not add independent timers, reconnect ownership,
  or tight-loop retries. Wi-Fi reconnect remains owned by `wifi_manager`.

A service-only outage while Wi-Fi remains continuously `ONLINE` is not polled
in Phase 12.3; transport/server reconnect policy is evaluated later in Phase
12.5/12.6 where the real chat lifecycle exists.

## Sensitive Logging Policy

Production Phase 12.3 code never logs activation codes, activation challenges,
transport tokens, credentials, private payloads, or raw server responses.
Temporary activation-code logging used during manual account binding was
removed at phase closure rather than left behind behind a disabled macro.

Safe diagnostics are limited to availability facts such as:

```text
Service reachable
MQTT available: yes/no
WebSocket available: yes/no
Activation code: present/none
Activation challenge: present/none
Activation timeout: <integer> ms
```

## Phase 12.3 Acceptance Evidence

Target-hardware evidence supplied during Phase 12.3 after account binding:

```text
XIAOZHI_FOUNDATION: Service reachable
XIAOZHI_FOUNDATION: MQTT available: yes
XIAOZHI_FOUNDATION: WebSocket available: yes
XIAOZHI_FOUNDATION: Activation code: none
XIAOZHI_FOUNDATION: Activation challenge: none
XIAOZHI_FOUNDATION: Activation timeout: 0 ms
XIAOZHI_FOUNDATION: Background service probe completed
```

This proves the service/info/activation path on the target board. It does not
prove chat transport selection, audio, lifecycle stress, or server-outage
recovery; those remain Phase 12.5 and 12.6 work.

## Phase 12.4.2–12.4.3 Configuration And NVS Execution Audit

The effective Phase 12 configuration is deliberately conservative:

| Setting | Effective value | Reason |
|---|---|---|
| `XIAOZHI_SYNC_SYSTEM_TIME_FROM_SERVER` | off | Xiaozhi information responses may report server time, but do not set the Gateway system clock. `time_manager` remains the owner of time policy. |
| `XIAOZHI_AUDIO_TASK_ALLOC_STATIC` | on | The chat audio worker uses the component file-scope static TCB and stack. |
| `XIAOZHI_AUDIO_TASK_ALLOC_DYNAMIC` | off | Mutually exclusive with static allocation. |
| `XIAOZHI_STACK_IN_PSRAM` | off | It is unavailable with static allocation; no Xiaozhi audio-task stack resides in PSRAM. |
| `AUDIO_MANAGER_PUBLIC_API_TEST` | off | The continuous audio stress coordinator remains a target-test-only feature. |
| `AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS` | 30 seconds | The `audio_manager` Kconfig default is retained; no Phase 12 override is present. |

`sdkconfig.defaults` contains only the Phase 12 Xiaozhi overrides needed to
preserve these choices (`XIAOZHI_SYNC_SYSTEM_TIME_FROM_SERVER=n` and static
audio-task allocation). It does not carry the unrelated audio stress switch or
manual-record-duration override that a `save-defconfig` operation can retain.

### NVS and cache-off policy

In pinned `esp_xiaozhi` 0.1.2, the default keystore calls IDF `nvs_*` APIs
directly in its caller context. The information path opens/writes `board`,
`mqtt`, and `websocket`; later MQTT/WebSocket start paths open their respective
namespaces for reads. The component offers an optional NVS-operations callback
layer, but this phase does not register it or add a project NVS service.

All project calls to a Xiaozhi lifecycle API that can reach this storage path
must execute from a task with an **internal-RAM stack**, never from a PSRAM-stack
task, ISR, or cache-disabled callback. This includes `get_info()` and future
`init`, `start`, `stop`, and `deinit` flows because their dependent transport
paths can access `board`, `mqtt`, or `websocket`. The current `xiaozhi_probe`
worker is created with `xTaskCreate`, so its normal ESP-IDF stack is internal.
The static Xiaozhi audio worker is also internal under the selected Kconfig.

No architecture defect requiring an NVS service was found for this configuration.
If a future lifecycle caller must use PSRAM stack storage, introduce and validate
the component NVS-operations service before that caller is allowed to reach a
Xiaozhi lifecycle API.

## Deferred Work

- Phase 12.5: MQTT+UDP versus WebSocket transport decision and failure behavior.
- Phase 12.6: init/start/connected/stop/deinit lifecycle matrix and repeated
  stress/fault testing.
