# Main Application Notes

## Purpose

`main.c` is the firmware composition root. It initializes platform, display,
storage, GUI, button, Wi-Fi, sensor, Firebase Authentication, and cloud components in
their required order. It also maps manager-owned snapshots into the GUI and
cloud data types without calling LVGL or HTTPS from producer callbacks.
The reusable component domain layout is documented in `components/README.md`.

## Current Startup Order

1. Log project identity.
2. Initialize NVS and `config_manager`.
3. Initialize ESP-NETIF and the default ESP event loop.
4. Initialize the LCD display driver and LVGL display integration.
5. Mount the SD card and register the LVGL `S:` filesystem.
6. Initialize `app_gui` and start its single UI task. No screen is selected
   until the coordinator resolves the final configuration state.
7. Optionally start the diagnostic `performance_monitor`.
8. Initialize `button_manager`, register its callback, and start its local
   polling task. Button failure does not stop unrelated service startup.
9. Initialize `wifi_manager` and register its status callback.
10. Initialize `app_network_coordinator` without scheduling its task.
11. Initialize `firebase_auth`, then initialize `cloud_manager` and register its
    status callback. The telemetry queue now exists, but TLS has not started.
12. Initialize `sensor_manager`, register its callback, and start DHT22
    sampling as a local service independent of network availability.
13. Schedule the dedicated one-shot `app_network_coordinator` task. It requests
    `BOOT` for a verified configured path or `PROVISIONING` directly for
    `NOT_CONFIGURED`. A configured device leaves `BOOT` after at most 60
    seconds even if stored Wi-Fi remains unavailable; the cached offline/retry
    snapshot is then shown on `WIFI_STATUS` without interrupting reconnect.
14. In the low-activity main loop, start `cloud_manager` only after the
    coordinator reaches `CONNECTING` for stored credentials or `ONLINE` after
    provisioning cleanup and adoption; retry task allocation after temporary
    memory pressure.

Startup errors are logged and return from `app_main()` instead of using active
`ESP_ERROR_CHECK()` calls that abort the firmware.

## Event Flow

```text
wifi_manager callback
    -> map runtime state to app_network_coordinator_wifi_event_t
    -> while provisioning, post CONNECTING_WIFI / WAITING_FOR_IP / FAILED only
    -> otherwise update coordinator CONNECTING / ONLINE / OFFLINE state
    -> verified normal ONLINE transition requests WIFI_STATUS
    -> publish has_ipv4 to cloud_manager's retained network epoch
    -> map wifi_manager_status_t to ui_wifi_status_t
    -> app_gui_post_wifi_status()

sensor_manager callback
    -> map sensor_manager_status_t to ui_sensor_status_t
    -> app_gui_post_sensor_status()
    -> map the same snapshot to cloud_sensor_telemetry_t
    -> cloud_manager_post_sensor_telemetry()

cloud_manager status callback
    -> map cloud_manager_status_t to ui_cloud_status_t
    -> app_gui_post_cloud_status()
    -> GUI task updates the Sensor screen Cloud row and indicator

button_manager task callback
    -> receive one copied PRESSED / RELEASED / LONG_PRESS event
    -> log Phase 7.1 diagnostics and return without blocking
    -> no NVS erase, provisioning restart, reboot, or LVGL call in Phase 7.1

provisioning_manager callback
    -> validate and hold credentials pending
    -> publish copied, non-sensitive progress outside the manager lock
    -> release them only after Wi-Fi connection success
    -> app_network_coordinator persists and verifies through config_manager
    -> wifi_manager adopts the active Station connection

app_network_coordinator task
    -> resolve persistent configuration state
    -> request BOOT or PROVISIONING through the GUI command queue
    -> connect stored credentials or run bounded BLE provisioning
    -> after BLE starts, copy its exact QR JSON into the GUI QR queue
    -> publish real progress through the GUI latest-value status queue
    -> persist/verify, stop/deinitialize BLE, and adopt the active connection
    -> clear session QR, show SUCCESS for 1500 ms, request WIFI_STATUS
    -> publish CONNECTING or ONLINE readiness for deferred cloud startup

app_main cloud gate
    -> reject READY / STARTING / RESOLVING_CONFIG / PROVISIONING / FAILED
    -> accept CONNECTING for stored Wi-Fi or ONLINE after provisioning handoff
    -> cloud_manager_start()

cloud task
    -> wake on telemetry or Wi-Fi epoch edges
    -> invalidate HTTP/TLS client on every changed network epoch
    -> classify transport, HTTP, authentication, and deterministic failures
    -> retry recoverable work with wakeable bounded backoff
```

The callbacks copy their input and return quickly. The GUI task owns LVGL
updates, while the cloud task owns authentication and HTTPS requests. The
cloud telemetry queue exists before the sensor task starts, so a sensor
callback cannot post into an uninitialized cloud component.

## Current Configuration

- `PERFORMANCE_MONITOR` is disabled at compile time.
- Sensor sampling period is 2000 ms with a 10000 ms stale timeout.
- Cloud successful-upload period is 10000 ms.
- Firebase token refresh margin is 300 seconds.
- The telemetry endpoint is
  `devices/esp32s3-001/latest.json` in Firebase Realtime Database.
- Wi-Fi credentials are loaded from `config_manager`; production code contains
  no hard-coded Wi-Fi SSID/password fallback.
- BLE provisioning waits up to 120 seconds for verified credentials. If that
  deadline expires with a credential handoff already in flight, it allows one
  additional 30-second connection grace. Framework cleanup polling remains
  finite.
- BLE provisioning allows at most three sessions including the initial
  session. Retryable terminal failures dwell for 1000 ms, clean fully to
  `STOPPED`, then use a 1500 ms `RETRYING` backoff before a new generation.
- BLE provisioning uses NimBLE, Security 1, five framework connection
  attempts, and the Espressif `v1`/`ble` QR schema.
- Provisioning progress uses dedicated generation-aware length-one overwrite
  queues. A normal credential failure leaves the same BLE session and QR
  available and does not consume the session retry budget.
- `sdkconfig.defaults` enables the 16 MB N16 flash layout, the custom
  partition table, BT/NimBLE, Security 1 support, required LVGL fonts, runtime
  statistics, the LVGL QR widget, and full cross-signed CA-bundle verification
  for current Google/Firebase TLS chains.
- The one-shot network coordinator task uses a 6 KB stack at priority 4.
- The 12 KB cloud task is allocated only after stored connection startup or
  successful provisioning cleanup and adoption.
- Cloud retry waits start at 5000 ms, cap at 60000 ms, and are wakeable by a
  network edge. A complete disconnect/reconnect during backoff advances the
  non-zero epoch and invalidates the previous HTTP/TLS client.
- `firebase_auth_init()` and `cloud_manager_init()` allocate only protected
  state and queues during startup; authentication and TLS remain deferred to
  the gated cloud task.
- Firebase device credentials remain development values compiled into source.
  They must move to protected local configuration before production or
  publication.

## Ownership And Threading

- `network_platform_init()` owns one-time NVS/config-manager, ESP-NETIF, and
  event-loop setup.
- Read-only config inspection never migrates implicitly. Boot attempts explicit
  migration once, then re-inspects before any credential load.
- `wifi_manager_connect()` runs only after config-manager has closed NVS and
  released its mutex. The application credential copy is then zeroized.
- `provisioning_manager` owns only transient BLE transport and credential
  handoff; `config_manager` remains the durable storage authority.
- `app_network_coordinator` owns boot and config-driven screen policy in a
  dedicated task. It may post non-blocking `app_gui` screen requests but does
  not call LVGL, create widgets, or render screens.
- `sensor_manager` is a local service and continues sampling and updating the
  GUI while provisioning waits, times out, or network connectivity is absent.
- The single Wi-Fi callback fans out a short runtime event to the coordinator,
  a non-blocking IPv4 snapshot to `cloud_manager`, and an independent GUI
  snapshot. `wifi_manager` remains responsible for connection and reconnect
  behavior. The cloud notification performs no HTTP/TLS work and safely
  retains edges before the cloud task exists. Provisioning events cannot
  promote coordinator state to `ONLINE` before persistence, cleanup, and
  adoption.
- The sensor callback can always post to an initialized cloud latest-value
  queue; posting does not perform authentication, TLS, or network I/O.
- The display handle has static lifetime because `ui_manager_lvgl` borrows it.
- `app_gui` owns the task that calls `lv_timer_handler()`.
- `app_gui` owns all screen construction, cleanup, rendering, transitions, and
  cached provisioning QR/Wi-Fi/sensor/cloud UI models. Its QR cache follows
  session invalidation rather than generic screen departure.
- Wi-Fi, sensor, cloud, provisioning, and coordinator callbacks must not call
  LVGL.
- Sensor callbacks must not perform Firebase authentication or HTTP requests.
- `firebase_auth` serializes network operations separately from short
  token/status state access; `cloud_manager` is its current network caller.
- There is no coordinated runtime shutdown path. Services are one-shot and run
  for the life of the firmware.

## Expected Cloud Logs

After Wi-Fi receives an IP address and sensor data becomes available:

```text
I (...) MAIN_APP: Cloud manager started after network handoff: state=...
I (...) FIREBASE_AUTH: Firebase Authentication sign-in successful
D (...) CLOUD_MANAGER: Publishing telemetry: T=... C, H=... %
D (...) CLOUD_MANAGER: Firebase HTTP status: 204
D (...) CLOUD_MANAGER: Telemetry published successfully
```

Firebase may return HTTP 200 instead of 204 depending on response options; all
HTTP 2xx statuses are accepted. The periodic request details are visible only
when the `CLOUD_MANAGER` log level allows debug output; failures remain warning
or error logs at the default level.

## Build

From the project root in an ESP-IDF 6.0.1 environment:

```powershell
idf.py build
```

Flash and monitor after selecting the actual serial port:

```powershell
idf.py -p <PORT> flash monitor
```

## Important Notes

- LVGL currently initializes before SD registration is checked. A failure in
  either path stops the remaining startup sequence.
- Missing Wi-Fi configuration starts BLE provisioning. Incomplete, invalid,
  interrupted-write, and unsupported data is preserved and does not
  auto-provision.
- Production startup does not erase the complete NVS partition to recover from
  NVS initialization errors.
- Cloud state and its latest-value telemetry queue are initialized before
  sensor sampling starts.
- Checkpoint 6.3.2 was hardware-accepted on 2026-07-26 using provisioning
  timeout, reset, reprovisioning, Wi-Fi adoption, Firebase upload, and GUI
  cloud-state recovery.
- Checkpoint 6.3.3 was hardware-accepted on 2026-07-26. Coordinator readiness
  now follows later Wi-Fi connecting, IPv4 online, disconnect, retry, and
  failure snapshots.
- Checkpoint 6.3.4 is implemented and build-verified. Hardware confirmation is
  still required for sensor/GUI operation during provisioning, provisioning
  timeout and late-DHCP recovery, watchdog stability, reconnect, and Firebase
  recovery.
- Phase 6.4.1 application screen orchestration is implemented with hardware
  testing pending. This does not mark Phase 6.4 complete.
- Phase 6.4.3 Espressif-compatible BLE provisioning QR rendering is
  implemented and build-verified. The user confirmed QR scanning and a
  successful provisioned Wi-Fi connection on target hardware; the final
  cross-phase regression matrix remains pending.
- Phase 6.4.4 real provisioning progress and verified success routing are
  implemented and build-verified. Hardware acceptance is pending for wrong
  credentials in the same BLE session, timeout cleanup, real-state visibility,
  the 1500 ms success dwell, and final dashboard routing. Phase 6.4 remains
  incomplete.
- Phase 6.4.5 bounded same-boot provisioning recovery is implemented with
  hardware testing pending. Static validation covers the three-session budget,
  `STOPPED -> READY` reinitialization, queue reuse/reset, generation filtering,
  terminal BLE memory release, and cloud gating. Phase 6.4 remains
  incomplete.
- Phase 6.4.6 cloud recovery is implemented with hardware testing pending.
  Static validation covers network epochs, task wakeups, HTTP-client reset,
  attempt classification, bounded 401/403 recovery, split Firebase Auth mutex
  ownership, latest telemetry retention, and best-effort ESP32-S3 BLE release.
  Final Phase 6.4 hardware acceptance remains pending.
- Phase 6.4.7 closure is implemented with hardware regression pending.
  `main` remains composition-only: one Wi-Fi callback fans copied snapshots to
  coordinator, GUI, and cloud; cloud task creation stays gated until stored
  connection startup or completed provisioning adoption. The project
  roadmap's A-N matrix is the final acceptance procedure.
- Firebase project setup and authenticated host testing are documented in
  `components/cloud/cloud_manager/README.txt` and `Test/TestFirebase_Auth.ps1`.
- Phase 7.1 button input was manually/hardware accepted by the user on
  2026-08-01. GPIO polling, debounce, press/release, and one-shot long-press
  detection are complete; reset execution and UI confirmation remain deferred.
- Never log or commit passwords, ID tokens, refresh tokens, service-account
  keys, or Firebase administrator credentials.

**Phase 6.4.7 — IMPLEMENTED / HARDWARE REGRESSION PENDING**

**Phase 6.4 — IMPLEMENTED / FINAL HARDWARE ACCEPTANCE PENDING**

## Future Attention

- Continue remaining Phase 6.3 work only when separately approved; checkpoint
  6.3.4 implementation does not authorize the next checkpoint.
- Move Firebase credentials out of source code.
- Add a coordinated application controller only when runtime stop/restart is
  required.
- Replace development demo hooks only when their bring-up role is finished.
