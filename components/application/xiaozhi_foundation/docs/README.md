# Xiaozhi Foundation — Phases 12.3–12.5

## Goal

Phase 12.3 validates the Xiaozhi service-information and activation path without
starting the chat/audio lifecycle. Phase 12.4 audits configuration, NVS, stack,
and cache-off behavior. Phase 12.5 records the transport decision: the project
uses **WebSocket only** for Xiaozhi and does not implement MQTT+UDP.

The project keeps the external component behind the `xiaozhi_foundation`
boundary and exposes only copied, non-sensitive scalar state.

## Runtime Flow

```text
Wi-Fi ONLINE
    -> app_network_coordinator
    -> xiaozhi_foundation_request_probe()
    -> private worker task
    -> esp_xiaozhi_chat_get_info()
    -> copy project-used non-sensitive flags
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
4. After reboot/binding, the probe again reached the service and activation code
   and challenge were absent.
5. `activation_timeout_ms` was `0` in the observed response. In the managed
   component this field is reset to zero before parsing and changes only when
   the server supplies a numeric timeout, so zero is treated as "no useful
   server-provided activation timeout" rather than a local failure.

The historical presence of MQTT capability in the server response does not
change the Phase 12.5 architecture decision. Project code no longer exposes or
selects MQTT for Xiaozhi.

## Ownership And Cleanup

`esp_xiaozhi_chat_get_info()` can allocate string fields in
`esp_xiaozhi_chat_info_t`. `xiaozhi_foundation_probe()` copies only booleans and
an integer into caller-owned storage, then calls `esp_xiaozhi_chat_free_info()`.
On a failed `get_info()` it still calls `free_info()` because partial parsing may
have allocated fields.

The public `xiaozhi_foundation_info_t` never contains:

- activation code or challenge strings;
- serial number or firmware URL strings;
- transport endpoints, tokens, usernames, passwords, or topics;
- any Xiaozhi-owned pointer or handle;
- MQTT availability or any project-side MQTT selection attribute.

## Private Xiaozhi Storage Audit

Phase 12.3 observed/audited these private namespaces owned by the managed
Xiaozhi component:

| Namespace | Purpose | Sensitivity | Project policy |
|---|---|---|---|
| `board` | persistent Xiaozhi UUID/device identity | identity | preserve |
| `mqtt` | upstream server-provided MQTT data | secret | upstream-owned, ignored by project, preserve |
| `websocket` | server-provided WebSocket URL/token/version | secret | preserve/private and use through `esp_xiaozhi` |

`esp_xiaozhi_chat_get_info()` may persist both MQTT and WebSocket response
objects when the server returns them. That is upstream component behavior. The
project does not read MQTT credentials, mirror them into `config_manager`, or
start the MQTT transport. The private upstream namespace is not considered a
project MQTT feature.

## Reset Policy

The current project long-press reset is a **Wi-Fi configuration reset**, not a
full device identity wipe. It clears project Wi-Fi credentials and Wi-Fi driver
persistence while preserving other device identity/configuration.

Phase 12 defines the Xiaozhi reset policy as:

- preserve upstream-owned `board`, `mqtt`, and `websocket` namespaces during the
  existing Wi-Fi reset;
- do not erase Xiaozhi namespaces from `app_reset_coordinator`;
- preserve the bound Xiaozhi UUID across Wi-Fi reprovisioning and normal reboot;
- if a future explicit "full device identity reset" is introduced, Xiaozhi
  unbind/credential erasure must be designed and verified separately before
  these namespaces are removed.

Preserving the unused upstream MQTT namespace avoids introducing unrelated
identity/storage side effects while still keeping MQTT completely outside the
project-selected transport architecture.

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
in Phase 12.3; WebSocket session reconnect and lifecycle behavior are validated
in the later Xiaozhi lifecycle phases.

## Sensitive Logging Policy

Production code never logs activation codes, activation challenges, transport
tokens, credentials, private payloads, or raw server responses. Temporary
activation-code logging used during manual account binding was removed at phase
closure rather than left behind behind a disabled macro.

Safe diagnostics are limited to project-relevant availability facts such as:

```text
Service reachable
WebSocket available: yes/no
Activation code: present/none
Activation challenge: present/none
Activation timeout: <integer> ms
```

MQTT availability is intentionally no longer copied or logged by
`xiaozhi_foundation`.

## Phase 12.3 Acceptance Evidence

Target-hardware evidence supplied during Phase 12.3 after account binding:

```text
XIAOZHI_FOUNDATION: Service reachable
XIAOZHI_FOUNDATION: WebSocket available: yes
XIAOZHI_FOUNDATION: Activation code: none
XIAOZHI_FOUNDATION: Activation challenge: none
XIAOZHI_FOUNDATION: Activation timeout: 0 ms
XIAOZHI_FOUNDATION: Background service probe completed
```

This proves the service/info/activation path on the target board. It does not
prove text/audio operation, lifecycle stress, or server-outage recovery.

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
directly in its caller context. The information path can open/write `board`,
`mqtt`, and `websocket` because those namespaces are controlled by the external
component. Project transport start is WebSocket-only and therefore only the
WebSocket transport path is intentionally exercised by Gateway code.

All project calls to a Xiaozhi lifecycle API that can reach storage must execute
from a task with an **internal-RAM stack**, never from a PSRAM-stack task, ISR,
or cache-disabled callback. This includes `get_info()` and future `init`,
`start`, `stop`, and `deinit` flows. The current `xiaozhi_probe` worker is created
with `xTaskCreate`, so its normal ESP-IDF stack is internal. The static Xiaozhi
audio worker is also internal under the selected Kconfig.

No architecture defect requiring an NVS service was found for this configuration.
If a future lifecycle caller must use PSRAM stack storage, introduce and validate
the component NVS-operations service before that caller is allowed to reach a
Xiaozhi lifecycle API.

## Phase 12.5 — Transport Decision

### Decision

**Selected transport: WebSocket only.**

MQTT+UDP is closed for the current project roadmap and will not be implemented,
validated further, or used as a runtime fallback.

### Evidence

The MQTT control path repeatedly failed before `CONNECTED` in the Gateway
integration with the signature:

```text
esp-x509-crt-bundle: Certificate validated
mqtt_client: transport_read(): EOF
mqtt_client: transport_read() error: errno=119
mqtt_client: mqtt_message_receive() returned -2
mqtt_client: MQTT connect failed
```

A separate standalone `xiaozhi_mqtt_test`, derived from the official
`esp_xiaozhi` flow and independent of Firebase, LVGL, `app_network_coordinator`,
and `xiaozhi_foundation`, reproduced the same pre-CONNECTED failure. Normal MQTT
retries also failed to obtain a `CONNECTED` event.

The test is sufficient for the architecture conclusion that the MQTT path is
not usable in the validated environment. It does **not** claim that the Xiaozhi
broker itself has been proven defective because broker/server-side logs are not
available.

### Project API consequence

- `xiaozhi_foundation_info_t` no longer exposes `mqtt_available`.
- `XIAOZHI_FOUNDATION_TRANSPORT_MQTT` is removed.
- `AUTO` remains only for caller compatibility and resolves to WebSocket.
- WebSocket availability remains the only transport capability exposed by the
  project boundary.
- The upstream `chat_config.has_mqtt_config` field is explicitly set to `false`
  before chat init. This is an external-component disable switch, not project
  MQTT support.
- No MQTT fallback is allowed.

### Remaining validation

Future Xiaozhi work validates only the selected WebSocket path, including text,
audio format/PCM integration, reconnect behavior, lifecycle stress, cleanup,
and resource measurements.

## Deferred / Closed Work

- MQTT+UDP Xiaozhi implementation: **closed / not selected** for the current
  roadmap.
- Phase 12.6+: WebSocket init/start/connected/stop/deinit lifecycle matrix and
  repeated stress/fault testing.
