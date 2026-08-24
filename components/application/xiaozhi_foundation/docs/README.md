# Xiaozhi Foundation — Phases 12.3–12.6

## Goal

Phase 12.3 validates the Xiaozhi service-information and activation path without
starting the chat/audio lifecycle. Phase 12.4 audits configuration, NVS, stack,
and cache-off behavior. Phase 12.5 records the transport decision: the project
uses **WebSocket only** for Xiaozhi and does not implement MQTT+UDP.
Phase 12.6 adds a default-off, project-owned controlled fault/recovery matrix
at safe public-API lifecycle boundaries. It is validation infrastructure, not
a voice feature or a substitute for real network/service fault evidence.

The project keeps the external component behind the `xiaozhi_foundation`
boundary and exposes only copied, non-sensitive scalar state.

## Temporary Validation Feature Gate

`Component config -> Xiaozhi Phase 12 validation -> Enable temporary Phase 12
Xiaozhi validation runtime` controls every automatic Phase 12 validation hook:

| Kconfig symbol | Default | Effect |
|---|---|---|
| `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE` | `n` | Keeps normal Gateway startup and Wi-Fi routing free of temporary Xiaozhi validation. |
| `CONFIG_XIAOZHI_FOUNDATION_P2F_EMBED_FIXTURE` | `n` | Available only when the master gate is enabled; embeds the lawful local P2-F fixture. |
| `CONFIG_XIAOZHI_FOUNDATION_P2F_E2E_ONLINE_VALIDATION` | `n` | Available only when the master gate and fixture option are enabled; selects P2-F instead of P2-E. |
| `CONFIG_XIAOZHI_FOUNDATION_P26_LIFECYCLE_MATRIX` | `n` | Available only when the master gate is enabled and P2-F is not selected; runs the isolated repeated P2.6 matrix instead of P2-E. |
| `CONFIG_XIAOZHI_FOUNDATION_P26_LIFECYCLE_CYCLE_COUNT` | `1` | Available only with the P2.6 matrix; requests 1-100 cycles and never starts a stress run during default boot. |
| `CONFIG_XIAOZHI_FOUNDATION_P26_FAULT_MATRIX` | `n` | Available only with the master gate enabled while P2-F and the repeated lifecycle matrix are unselected; enables the controlled P2.6 fault/recovery selector. |
| `CONFIG_XIAOZHI_FOUNDATION_P26_FAULT_SELECTOR_*` | `NONE` | Selects one safe project-owned abort boundary or `ALL_SUPPORTED`. `NONE` performs no injection and leaves the regular P2-E checkpoint selected. |

This is temporary Phase 12 infrastructure, not a production voice-assistant
switch. With the master gate disabled, boot, stored-Wi-Fi recovery,
provisioning completion, and later `OFFLINE -> ONLINE` transitions retain the
pre-Xiaozhi application routes: no validation worker, service probe, observer
registration, or automatic `XIAOZHI` screen is requested.

To run P2-E, enable only the master gate. To run P2-F, enable the master gate,
provide the documented lawful fixture, then enable both P2-F sub-options. To
run a controlled P2.6 fault case, enable the master gate and fault-matrix
option while leaving P2-F and the repeated lifecycle matrix unselected, then
select exactly one boundary. Progress to individually selected further cases
or `ALL_SUPPORTED` only after the preceding target result is clean. The
validation implementation and public `xiaozhi_foundation` APIs remain compiled
for this project; the application composition layer simply makes no automatic
request while the gate is off. `app_network_coordinator` intentionally retains
its `xiaozhi_foundation` build dependency so feature-on composition remains
simple and reviewable; this dependency creates no feature-off runtime call.

## Runtime Flow

```text
Master gate disabled (default):
boot -> existing application init -> Wi-Fi ONLINE
    -> existing cloud/network/sensor/audio behavior
    -> no Xiaozhi validation worker or automatic Xiaozhi screen

Master gate enabled:
Wi-Fi ONLINE -> app_network_coordinator
    -> xiaozhi_foundation_request_transport_validation()
    -> P2-E by default, P2-F with its fixture gate, repeated P2.6,
       or one explicitly selected P2.6 fault/recovery matrix
    -> optional copied-status observer and temporary Xiaozhi screen
```

`xiaozhi_foundation_request_probe()` remains a non-blocking public diagnostic
API, but Gateway composition does not automatically call it. Its worker may
perform HTTP/TLS work and can wait up to the component-configured timeout, so
future explicit callers must schedule it instead of executing it in Wi-Fi
callbacks or coordinator event paths.

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

### P2-D — WebSocket Text Protocol Receive Validation

The exact resolved dependency is `espressif/esp_xiaozhi` **0.1.2**. Its public
`esp_xiaozhi_chat.h` defines:

- `ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT`, whose `event_data` is an
  `esp_xiaozhi_chat_text_data_t *`;
- `ESP_XIAOZHI_CHAT_TEXT_ROLE_USER` and
  `ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT`;
- `ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE`,
  `ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR`, and
  `ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI`.

The pinned public header exposes chat lifecycle, audio-channel, audio-data,
wake-word, listening, and abort-speaking APIs, but **does not expose an API for
arbitrary typed user-text transmission** such as `send_text(chat, prompt)`.
`esp_xiaozhi_chat_open_audio_channel()` accepts an optional protocol hello
message only as part of opening an audio channel; it is not a typed-text chat
prompt API and P2-D does not call it. `esp_xiaozhi_transport_send_text()` is a
private component implementation detail and is not used by project code.

P2-D therefore records:

```text
Public arbitrary text TX: NOT AVAILABLE
CHAT_TEXT receive handler: IMPLEMENTED
End-to-end USER/ASSISTANT CHAT_TEXT evidence: DEFERRED TO P2-F AUDIO/STT
```

The temporary WebSocket validation configures both
`chat_config.event_callback` and `chat_config.event_callback_ctx`. Its protocol
callback is deliberately small:

- validates every event and payload pointer before use;
- casts `CHAT_TEXT` only to `esp_xiaozhi_chat_text_data_t *` and separates
  `USER` from `ASSISTANT` data;
- treats every upstream callback pointer as borrowed and never retains it;
- copies each role's latest text to a separate application-owned **192-byte**
  buffer, including NUL termination (at most 191 payload bytes); text beyond
  that bound is deterministically truncated and marked as such;
- records bounded `CHAT_TTS_STATE`, `CHAT_ERROR` source/code, and `CHAT_EMOJI`
  diagnostics, then sets EventGroup fact bits;
- does not call LVGL, provisioning, Wi-Fi/config/NVS/Firebase operations,
  hardware APIs, or Xiaozhi lifecycle APIs.

The worker, not the callback, owns timeout decisions, lifecycle cleanup, and
diagnostic logs. It logs only role, copied length, truncation status, TTS state,
error code/source, and emoji length. It never logs text content, tokens,
credentials, endpoint values, or raw server payloads. The callback context and
EventGroup remain valid through `chat_deinit()`; copied text is then cleared
before the worker exits.

P2-D itself does not add a project text-send abstraction, audio encoder,
`audio_manager` dependency, microphone, speaker, LVGL, or production voice
state machine. The accepted P2-C WebSocket-only connection evidence is retained;
the temporary Phase 12.5 worker now extends the selected connection into the
separate P2-E or explicitly opted-in P2-F validation checkpoint.

### P2-D Hardware Acceptance

No P2-D hardware pass is claimed by this implementation. The pinned public API
cannot produce an arbitrary typed-text prompt in isolation, so an actual
`CHAT_TEXT role=USER` / `CHAT_TEXT role=ASSISTANT` serial trace remains deferred
to the supported P2-F audio/STT interaction path. A future P2-F pass must show
the received role/length diagnostics while preserving the WebSocket-only
lifecycle and no secret logging.

### P2-E - WebSocket Audio-Channel Lifecycle

**Implementation status:** implemented and ESP-IDF 6.0.1 build-verified.
**Hardware status:** pending real serial evidence.

When the master validation gate is enabled and P2-F is not selected, the
validation requested after the coordinator reaches `ONLINE` is P2-E:

```text
CONNECTED
  -> open_audio_channel(opus, 16000 Hz, mono, 60 ms)
  -> AUDIO_CHANNEL_OPENED
  -> 1000 ms bounded hold
  -> close_audio_channel()
  -> AUDIO_CHANNEL_CLOSED
  -> chat_stop() -> chat_deinit() -> destroy MCP
```

The worker waits at most 15 seconds for `CONNECTED` and
`AUDIO_CHANNEL_OPENED`, and at most 8 seconds for `AUDIO_CHANNEL_CLOSED`.
`DISCONNECTED` and protocol `CHAT_ERROR` immediately fail the active wait. If
an audio channel was opened when another step fails, cleanup still attempts a
bounded close before stopping/deinitializing chat. The original failure remains
the reported result if cleanup also fails.

In pinned source, `esp_xiaozhi_chat_open_audio_channel()` sends the hello,
waits internally for the server hello (up to 10 seconds), opens the local
WebSocket audio state, then posts `AUDIO_CHANNEL_OPENED`. Closing clears local
session state and posts `AUDIO_CHANNEL_CLOSED`. These are real component event
loop observations, but they are **not** independent server-originated media
acknowledgements with payload. P2-E therefore proves the exposed 0.1.2
lifecycle contract; it does not by itself prove ASR or server audio media.

Expected successful evidence includes:

```text
=== P2-E WEBSOCKET AUDIO CHANNEL ===
WebSocket connected
Opening audio channel
open_audio_channel: OK
AUDIO_CHANNEL_OPENED
Audio channel stable for 1000 ms
Closing audio channel
close_audio_channel: OK
AUDIO_CHANNEL_CLOSED
chat_stop: OK
chat_deinit: OK
P2-E RESULT: PASS
```

Hardware PASS requires the complete trace above, no disconnect/protocol error,
no crash/watchdog, and no reconnect/session activity after complete deinit.
No P2-E hardware pass is claimed until that trace is supplied.

### P2-F - Known-Audio WebSocket E2E

**Implementation status:** implemented as validation-only infrastructure and
default-config build-verified. **Hardware status:** pending a legal local
fixture and real serial evidence.

No reusable speech asset exists in this repository. P2-F therefore does not
invent PCM, random bytes, or a copyrighted sample. It is disabled by default
and has no production dependency. To enable it, place a lawful, non-sensitive
local fixture at `test_assets/p2f_fixture.bin`, then enable the master option
and both P2-F menuconfig options under `Component config -> Xiaozhi Phase 12
validation`:

```text
Enable temporary Phase 12 Xiaozhi validation runtime = y
Embed the optional P2-F known-audio fixture = y
Run P2-F known-audio E2E instead of default P2-E = y
```

The fixture format is documented in `test_assets/README.md`. It is an `XZF1`
container of one raw **Opus** packet per 60 ms record: 16 kHz, mono, at most
120 frames, 2048 bytes/frame, and 64 KiB total. It is not WAV, PCM, Ogg, WebM,
or a decoder input. The test sends exactly one record through
`esp_xiaozhi_chat_send_audio_data()` and delays approximately 60 ms before the
next record; it never sends the whole utterance as one blob. The parser checks
the declared container fields and bounds, but cannot decode an Opus packet to
prove its duration; the fixture creator must guarantee the 60 ms contract.

The P2-F test first parses and bounds-checks every fixture record, opens its
own WebSocket audio channel, sends `listen/start` with mode `manual`, streams
the fixture, sends `listen/stop`, then waits at most 30 seconds for all of:

- non-empty `USER` `CHAT_TEXT`;
- non-empty `ASSISTANT` `CHAT_TEXT`;
- at least one non-empty `audio_callback` payload.

It reports `TTS START`, `SENTENCE_START`, and `STOP` when supplied by the
pinned protocol, but audio callback count/total bytes are the hard audio
response evidence. The test does not play, decode, retain, log, or forward raw
audio. P2-F only logs bounded, printable-safe copies of the USER/ASSISTANT
text for human inspection of the known fixture interaction; all other Phase
12.5 paths retain the P2-D rule of never logging text content.

The exact pinned source contract is:

- The hello contains caller-supplied `format`, `sample_rate`, `channels`, and
  `frame_duration`; the defaults are `opus`, 16000, 1, and 60 respectively.
- `format = "pcm"` passes only the source's non-empty-string validation. The
  public `audio_type` enum/config has only `OPUS`, server hello stores only
  sample rate/frame duration, and no PCM encoder/validation path exists.
  **PCM direct supported for this validation: no.**
- `esp_xiaozhi_chat_send_audio_data()` performs no encoding. For WebSocket it
  sends the caller's buffer as one binary WebSocket frame, so the caller must
  provide an already encoded complete Opus packet. Source does not enforce
  packet duration/boundaries; P2-F fixes its fixture contract to 60 ms packets
  and applies real-time pacing.
- The WebSocket binary handler forwards raw server bytes directly to
  `audio_callback`; it does not decode them. With P2-F's `opus` hello, the
  expected RX is raw Opus payload, but the public API does not expose a
  negotiated RX-format object.
- `ESP_XIAOZHI_CHAT_EVENT_AUDIO_DATA_INCOMING` is declared in the 0.1.2 public
  header but is not posted by its pinned implementation. P2-F registers the
  event defensively and uses the supported `audio_callback` counters for the
  actual RX criterion.

The P2-F completion trace must show `start_listening: OK`, non-zero TX frames
and bytes, `stop_listening: OK`, non-empty USER and ASSISTANT diagnostics,
non-zero audio RX counters, `AUDIO_CHANNEL_CLOSED`, successful stop/deinit, and
`P2-F RESULT: PASS`. Human review may accept recognizably related transcript
text; byte-for-byte ASR/LLM wording is not required. No P2-F hardware PASS is
claimed without that serial trace.

### P2.1 — Temporary Interaction Status UI

**Implementation status:** build-verified temporary validation presentation.
**Hardware status:** pending target-LCD and P2-E/P2-F serial evidence.

The foundation adds one optional observer API,
`xiaozhi_foundation_register_ui_status_callback()`. The application composition
layer registers it once before network orchestration only when the master
validation gate is enabled; it is immutable while a probe or transport
validation is active. With the gate disabled, no observer is registered. The
foundation borrows the callback/context for firmware lifetime; the observer
must copy the borrowed snapshot before returning.

```text
foundation worker / Xiaozhi event callback
    -> copied xiaozhi_foundation_ui_status_t
    -> main copies to ui_xiaozhi_status_t
    -> app_gui length-one latest-value queue
    -> app_gui UI task and optional XIAOZHI screen
```

The snapshot exposes only `DISCONNECTED`, `READY`, `LISTENING`, `PROCESSING`,
`RESPONDING`, or `ERROR`; monotonic listening start/stop times; a non-sensitive
`esp_err_t`; and separate copied USER/ASSISTANT text buffers. Each buffer is
192 bytes including NUL and is copied from the existing P2-D protocol storage.
It contains no raw audio, endpoint, token, activation material, framework
pointer, or raw error-source string.

`LISTENING` begins only after the supported P2-F `send_start_listening()` call
succeeds. The worker records its stop time after supported
`send_stop_listening()` succeeds and publishes `PROCESSING`; an early
USER/ASSISTANT/TTS response advances the UI to `RESPONDING`, and TTS stop may
return it to `READY`. `CONNECTED` publishes `READY`; `DISCONNECTED` publishes
the safe disconnected state; protocol or lifecycle failure publishes `ERROR`.
An error is retained through asynchronous disconnect cleanup. The GUI, not the
foundation, converts the monotonic timestamps to a periodically displayed
duration.

The observer is a presentation bridge, not a new voice lifecycle. It does not
change WebSocket selection, MQTT policy, audio-channel ownership, worker
priority, P2-D receive behavior, P2-E/P2-F validation steps, or cleanup.
It never calls LVGL, starts a screen, sends per-frame/timer updates, starts a
FreeRTOS task, or touches hardware. Screen navigation does not start, stop, or
cancel a validation operation. This temporary UI must not be confused with the
later project-owned `voice_assistant`/production GUI integration.

### Callback and cleanup ownership

Both `event_callback` and `audio_callback` receive borrowed source pointers.
The callbacks only perform bounded copies or scalar counter updates under the
short `portMUX` critical section and set EventGroup facts. They do not block,
allocate, call lifecycle/Wi-Fi/provisioning/LVGL/Firebase/hardware APIs, or
retain text/audio/error pointers. The worker owns all waits, logging,
open/close/start/stop/deinit decisions, handler unregistration, EventGroup
deletion, MCP destruction, and zeroization of copied text before exit.

### P2.3 — Bounded Resource And Lifecycle Diagnostics

**Implementation status:** default-off and master-gate/P2-E host builds pass
on ESP-IDF 6.0.1.
**Hardware status:** no P2-E/P2-F resource or lifecycle result is claimed.

When `CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE` is enabled, the one-shot
validation worker captures low-frequency snapshots at `BEFORE_XIAOZHI`, after a
successful `chat_init`, after `CONNECTED`, `AFTER_VALIDATION`, and
`AFTER_CLEANUP`. It uses the established ESP-IDF heap-capability APIs:

- `heap_caps_get_free_size()`;
- `heap_caps_get_minimum_free_size()`;
- `heap_caps_get_largest_free_block()`.

Each snapshot separately reports Internal (`MALLOC_CAP_INTERNAL |
MALLOC_CAP_8BIT`), DMA-capable (`MALLOC_CAP_DMA`), and PSRAM
(`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`) free/minimum/largest-block bytes plus
the worker's `uxTaskGetStackHighWaterMark(NULL)` result in FreeRTOS words.
DMA-capable memory overlaps Internal memory on this target, so the two values
are never added together. Minimum free is the capability pool's boot-lifetime
low-water mark; it is not an interval-only measurement.

The final worker summary reports `AFTER_CLEANUP - BEFORE_XIAOZHI`, saturating
per-operation lifecycle/protocol counters, the selected P2-E/P2-F result, and
separate first primary and cleanup error codes. A negative free-byte delta is a
signal to investigate across repeated cycles; one transaction cannot prove a
memory leak. CPU utilization, socket count, TLS allocation size, and packet
loss are not directly observable from this bounded snapshot and are not
invented.

Counter updates remain callback-safe: callbacks only make bounded copies or
saturating scalar updates under the existing short `portMUX` critical section,
then set EventGroup facts. The worker alone prints snapshots and summaries; no
diagnostic task, timer, per-audio-frame log, or production telemetry is added.
With the master gate disabled, the application makes no validation request, so
this diagnostic has no runtime execution or background overhead.

The transport decision is recorded in
[the WebSocket ADR](../../../../docs/ADR_XIAOZHI_WEBSOCKET_TRANSPORT.md).
Required target evidence is defined by the
[hardware acceptance data contract](../../../../docs/XIAOZHI_HARDWARE_ACCEPTANCE.md).

### P2.6 - WebSocket Lifecycle And Controlled Fault/Recovery Matrix

**Implementation scope:** default-off, validation-only WebSocket lifecycle and
controlled recovery evidence. The fault matrix aborts only the project's own
continuation at a safe public-API boundary with `ESP_ERR_INVALID_STATE`; it
does not force an upstream allocation, mutate a private `esp_xiaozhi` object,
or simulate a network outage.

The earlier repeated-lifecycle progression reached 1/1, 3/3, 10/10, 20/20, and
100/100 passed cycles with no captured panic/watchdog/assert or raw payload-tag
output. Its 100-cycle post-cleanup Internal free/largest-block decline remains
an investigation signal and blocks resource-stability acceptance. The first
controlled HIL run, `AFTER_CHAT_INIT`, correctly emitted one each of
`XZ_FAULT_BEGIN`, `XZ_FAULT_INJECT`, `XZ_FAULT_EXPECTED`, successful cleanup,
fresh recovery, successful end, and `=== XIAOZHI FAULT SUMMARY ===`. It had no
captured panic/watchdog/assert/stale-callback marker or upstream raw-payload
tag. Its recovery-after-cleanup boundary was nevertheless 21,420 bytes lower
in Internal free and 32,768 bytes lower in largest Internal block than the
fault-before boundary. That consistent resource trend is **BLOCKED** pending a
repeatable source-specific audit; no further subset/full fault escalation is
claimed and no project memory leak is asserted.

Enable the master gate and select either the repeated lifecycle matrix or the
fault matrix; P2-F and those two P2.6 modes are mutually exclusive. The fault
selector defaults to `NONE`, which injects nothing and leaves P2-E selected.
For any actual selected case, run exactly one fault lifecycle, inspect its
cleanup, then run one fresh normal P2-E recovery lifecycle. The runner stops
on the first unexpected result or failed recovery; it does not retry, reconnect
Wi-Fi, or fall back to MQTT.

Each cycle owns a fresh validation context, EventGroup, event-handler instance,
chat handle, MCP engine, callback counters, error state, generation value, and
resource snapshots. A context remains valid through handler unregistration and
`chat_deinit()`; the handler is unregistered before deinit, then no old event
group, handle, callback storage, or generation is reused by recovery. This
uses the pinned public lifetime contract rather than retaining a callback after
its owning context has been destroyed.

The normal P2-E path is:

```text
get_info -> WebSocket-only init -> start -> CONNECTED
    -> open audio -> AUDIO_CHANNEL_OPENED -> bounded hold
    -> close audio -> AUDIO_CHANNEL_CLOSED -> stop -> deinit -> destroy MCP
```

The controlled cases deliberately return at these project-owned continuation
boundaries after the preceding acquisition succeeded:

| Selector | Shared cleanup ownership after injected return |
|---|---|
| `AFTER_GET_INFO` | `esp_xiaozhi_chat_free_info()` releases potentially partial info. |
| `AFTER_MCP_CREATE` | Destroy the new MCP engine. |
| `AFTER_EVENT_GROUP_CREATE` | Delete the EventGroup, then destroy MCP. |
| `AFTER_CHAT_INIT` | Deinit chat, destroy MCP, delete EventGroup. |
| `AFTER_EVENT_HANDLER_REGISTER` | Unregister handler before chat deinit, then destroy/delete. |
| `AFTER_CHAT_START` | Stop, unregister, deinit, destroy, then delete. |
| `AFTER_AUDIO_CHANNEL_OPEN` | Close the observed audio channel first, then the ordered stop/unregister/deinit/destroy/delete cleanup. |

The first primary error is retained as the root cause. A cleanup failure is
recorded separately and never overwrites it. Each selected case logs
`XZ_FAULT_BEGIN`, `XZ_FAULT_INJECT`, expected/unexpected result,
`XZ_FAULT_CLEANUP`, `XZ_FAULT_RECOVERY`, `XZ_FAULT_END`, resource samples, and
the bounded aggregate `=== XIAOZHI FAULT SUMMARY ===` / `XZ_FAULT_RESULT`.
Internal and DMA-capable pools overlap and are never added; heap snapshots are
an investigation indicator rather than leak, CPU, socket, TLS-allocation, or
packet-loss proof.

#### Pinned API safety classification

This matrix audits the resolved public `esp_xiaozhi` 0.1.2 contract and uses
only the following categories:

| API or operation | Classification | P2.6 action |
|---|---|---|
| `esp_xiaozhi_chat_init()` with a new config/handle | SAFE_AND_DEFINED | One new chat instance per cycle. The upstream single-instance check is not raced or bypassed. |
| `esp_xiaozhi_chat_start()` after valid init/MCP configuration | SAFE_AND_DEFINED | One start per new chat instance. |
| `ESP_XIAOZHI_CHAT_EVENTS` handler register/wait/unregister | SAFE_AND_DEFINED | New EventGroup and handler registration per cycle; callback context remains live through `chat_deinit()`. |
| `esp_xiaozhi_chat_open_audio_channel()` / close after successful connection | SAFE_AND_DEFINED | Reuses the bounded P2-E lifecycle exactly once per normal or recovery cycle. |
| `esp_xiaozhi_chat_stop()` then `esp_xiaozhi_chat_deinit()` | SAFE_AND_DEFINED | One ordered cleanup per cycle; no stopped handle is restarted. |
| Project continuation after the safe selector boundaries above | SAFE_EXPECTED_ERROR | Deterministic `ESP_ERR_INVALID_STATE`; shared cleanup must succeed before fresh recovery. |
| `ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE` | SOURCE_AUDITED | Public event is made a sticky runtime failure that wakes bounded waits; no fake goodbye is generated. |
| `get_info()` malformed response / upstream allocation failure | SOURCE_AUDITED | Public errors and project allocation checks are preserved; no global heap exhaustion or private allocator hook is forced. |
| Real Wi-Fi/AP loss, DNS/TLS/server loss, or server timeout | EXTERNAL_HARDWARE_REQUIRED | Not simulated by project code and not given to Wi-Fi/provisioning ownership. |
| listening start/stop | SAFE_AND_DEFINED only in its established open P2-F session | Not exercised because P2-F fixture/audible proof remains pending. |
| double close, double deinit, use-after-deinit, start-after-stop, forced free, private transport APIs, raw protocol messages | UNSAFE_OR_UNDEFINED | Never tested. |

The duplicate-validation-request check remains a separate project-gate
expected-error case. P2.6 does not add Wi-Fi-loss simulation, raw protocol
messages, production microphone/speaker wiring, typed-text send, a production
voice state machine, or a transport fallback.

#### Runtime payload-log containment

Pinned upstream chat/MCP tags can emit raw incoming protocol payloads at
runtime. While a temporary validation cycle is active, the foundation saves
the configured levels and sets `ESP_XIAOZHI_CHAT`, `esp_mcp_mgr`, and
`esp_mcp_engine` to `ESP_LOG_NONE`. The worker's own `XIAOZHI_FOUNDATION`
logs retain lifecycle stage, safe error code, bounded counters, and resource
facts. After chat deinit/MCP cleanup, the original upstream tag levels are
restored. This uses the project's enabled `CONFIG_LOG_DYNAMIC_LEVEL_CONTROL`;
if a future configuration disables that ESP-IDF feature, validation must not
be used until an equally secret-safe logging boundary is provided.

### Remaining validation

P2-E/P2-F hardware acceptance, feature-off target-hardware observation, the
source-specific explanation of the observed P2.6 resource trend, controlled
fault subset/full HIL escalation, and real Wi-Fi/AP, Internet, DNS/TLS, and
service-fault evidence remain pending. MQTT remains closed; no MQTT fallback,
production voice assistant, microphone, speaker, or production GUI integration
is part of this validation layer. P2.1 is only a temporary copied-status
display for the existing validation worker.

## Deferred / Closed Work

- MQTT+UDP Xiaozhi implementation: **closed / not selected** for the current
  roadmap.
- Phase 12.6 target HIL: resource-stability audit before any more fault-matrix
  escalation or phase-close claim.
- Separate hardware scope: real Wi-Fi/AP loss, Internet/DNS/TLS/service loss,
  server goodbye, malformed remote response, and timeout evidence. None is
  replaced by a project-owned fake transport fault.
