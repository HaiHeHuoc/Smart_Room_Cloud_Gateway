# Main Application Notes

## Purpose

`main.c` is the firmware composition root. It initializes platform, display,
storage, GUI, Wi-Fi, sensor, Firebase Authentication, and cloud components in
their required order. It also maps manager-owned snapshots into the GUI and
cloud data types without calling LVGL or HTTPS from producer callbacks.
The reusable component domain layout is documented in `components/README.md`.

## Current Startup Order

1. Log project identity and optionally start `performance_monitor`.
2. Initialize NVS and `config_manager`, inspect Wi-Fi state, and migrate a
   supported legacy schema once when required.
3. Initialize ESP-NETIF and the default ESP event loop.
4. Initialize the LCD display driver and LVGL display integration.
5. Mount the SD card and register the LVGL `S:` filesystem.
6. Initialize `app_gui`, start its single UI task, and create the Wi-Fi screen.
7. Initialize `wifi_manager`, register its callback, and load/connect only
   when persisted Wi-Fi configuration is `VALID`.
8. Initialize `sensor_manager`, register its callback, and start DHT22
   sampling.
9. Initialize `firebase_auth` with Email/Password credentials and the expected
   device UID.
10. Initialize and start `cloud_manager` with the Firebase latest-value URL.
11. Leave `app_main()` in a low-activity delay loop while component tasks own
    ongoing work.

Startup errors are logged and return from `app_main()` instead of using active
`ESP_ERROR_CHECK()` calls that abort the firmware.

## Event Flow

```text
wifi_manager callback
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
```

The callbacks copy their input and return quickly. The GUI task owns LVGL
updates, while the cloud task owns authentication and HTTPS requests.

## Current Configuration

- `PERFORMANCE_MONITOR` is disabled at compile time.
- Sensor sampling period is 2000 ms with a 10000 ms stale timeout.
- Cloud successful-upload period is 10000 ms.
- Firebase token refresh margin is 300 seconds.
- The telemetry endpoint is
  `devices/esp32s3-001/latest.json` in Firebase Realtime Database.
- Wi-Fi credentials are loaded from `config_manager`; production code contains
  no hard-coded Wi-Fi SSID/password fallback.
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
- The display handle has static lifetime because `ui_manager_lvgl` borrows it.
- `app_gui` owns the task that calls `lv_timer_handler()`.
- Wi-Fi and sensor callbacks must not call LVGL.
- Sensor callbacks must not perform Firebase authentication or HTTP requests.
- `firebase_auth` serializes token state; `cloud_manager` is its current network
  caller.
- There is no coordinated runtime shutdown path. Services are one-shot and run
  for the life of the firmware.

## Expected Cloud Logs

After Wi-Fi receives an IP address and sensor data becomes available:

```text
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
- Missing Wi-Fi configuration is logged and startup continues without a
  connection request. Incomplete, invalid, interrupted-write, and unsupported
  data is preserved.
- Production startup does not erase the complete NVS partition to recover from
  NVS initialization errors.
- Sensor sampling starts before cloud initialization, but its initial DHT22
  stabilization delay normally allows cloud resources to become ready first.
  A telemetry post before cloud initialization is safely rejected.
- Firebase project setup and authenticated host testing are documented in
  `components/cloud/cloud_manager/README.txt` and `Test/TestFirebase_Auth.ps1`.
- Never log or commit passwords, ID tokens, refresh tokens, service-account
  keys, or Firebase administrator credentials.

## Future Attention

- Add Sprint 6 provisioning to populate missing Wi-Fi configuration.
- Move Firebase credentials out of source code.
- Add a coordinated application controller only when runtime stop/restart is
  required.
- Replace development demo hooks only when their bring-up role is finished.
