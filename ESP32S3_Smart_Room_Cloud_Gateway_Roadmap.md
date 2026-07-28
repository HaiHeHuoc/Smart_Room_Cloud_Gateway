# ESP32-S3 Smart Room Cloud Gateway — Roadmap & Tracking

**Project name:** ESP32-S3 Smart Room Cloud Gateway  
**Target board:** ESP32-S3  
**Main stack:** ESP-IDF, FreeRTOS, Wi-Fi, BLE Provisioning, LVGL, Firebase Realtime Database  
**Created date:** 2026-06-30  
**Owner:** Trần Long Hải  

---

## 1. Project Goal

Build a practical ESP32-S3 IoT device that can:

1. Use **BLE** to configure Wi-Fi credentials.
2. Connect to the internet through **Wi-Fi Station mode**.
3. Display local device status and sensor data on an **LCD using LVGL**.
4. Read sensor data such as temperature/humidity.
5. Upload latest and historical data to a cloud service, initially **Firebase Realtime Database**.
6. Store configuration locally using **NVS**.
7. Support factory reset through a physical button.
8. Be structured as a maintainable embedded project with components, state machine, event flow, logs, and documentation.

This project follows a **project-driven learning approach**: learn ESP-IDF topics just-in-time while building real features.

---

## 2. Final Product Vision

The final device should behave like a small smart room gateway:

```text
First boot / no Wi-Fi config
    ↓
Start BLE provisioning
    ↓
Phone sends Wi-Fi SSID/password
    ↓
ESP32 stores credentials in NVS
    ↓
ESP32 connects to Wi-Fi
    ↓
LCD shows device status, IP, sensor values, cloud sync status
    ↓
ESP32 periodically uploads sensor data to Firebase
```

---

## 3. Core Features

| Feature | Priority | Status | Notes |
|---|---:|---|---|
| LCD bring-up | P0 | Done | ST7735 / SPI hardware-accepted |
| LVGL UI | P0 | Done | Queue-driven screens and labels |
| Wi-Fi Station | P0 | Done | Event-driven Station connection and status UI |
| Sensor reading | P0 | Done | DHT22 manager and stale/error handling |
| Firebase upload | P0 | Done | Authenticated Realtime Database REST PUT verified on hardware |
| NVS config storage | P0 | Done | Integrity, persistence, migration, and recovery tests accepted |
| BLE Wi-Fi provisioning | P1 | In progress | Phase 6.4 implemented; final A-N hardware acceptance pending |
| Button factory reset | P1 | Not started | Long press to erase config |
| Wi-Fi reconnect strategy | P1 | Done | Event-driven exponential-backoff reconnect owned by `wifi_manager` |
| Cloud retry queue | P1 | Done | Latest-value queue with bounded retry backoff |
| MQTT | P2 | Not planned | Optional future feature |
| OTA | P2 | Not planned | Final-stage optional feature |
| Custom mobile app | P3 | Not planned | Avoid early scope creep |

Status values:

```text
Not started / In progress / Blocked / Done / Deferred
```

**Current milestone (2026-07-28):** Phase 6.4 provisioning UI, bounded session
recovery, and cloud recovery are implemented. Static closure validation is
tracked by Phase 6.4.7; the final A-N target-hardware matrix below remains
pending before Phase 6.4 can be marked complete.

---

## 4. Recommended MVP Scope

The first realistic version should include:

```text
BLE Provisioning
+ Wi-Fi Station
+ LVGL LCD Dashboard
+ Sensor Monitor
+ Firebase Realtime Database Upload
+ NVS Config Storage
+ Button Factory Reset
```

Do **not** add these too early:

```text
Custom mobile app
Firestore direct integration
OTA
MQTT
WebSocket dashboard
BLE always-on streaming
Complex local web dashboard
```

---

## 5. Hardware Plan

| Hardware | Purpose | Required for MVP? | Notes |
|---|---|---:|---|
| ESP32-S3 board | Main controller | Yes | Prefer board with PSRAM if available |
| ST7735 LCD 128x160 | Local UI | Yes | SPI display |
| DHT22 | Temperature/humidity sensor | Yes | Can replace with better sensor later |
| Button | Factory reset / menu input | Yes | Long press detection |
| LED / RGB LED / WS281x | Status indicator | Optional | Useful for quick debugging |
| USB cable | Flash/log monitor | Yes | Use `idf.py monitor` |

---

## 6. Software Architecture

### 6.1 Component Structure

```text
components/
├── cloud/
│   ├── cloud_manager/
│   └── firebase_auth/
├── connectivity/
│   ├── provisioning_manager/
│   └── wifi_manager/
├── display/
│   ├── display_driver/
│   └── waveshare__esp_lcd_st7735/
├── sensing/
│   ├── sensor_manager/
│   └── sensor_DHT22/
├── storage/
│   ├── config_manager/
│   └── sd_card_manager/
├── system/
│   ├── common/
│   └── performance_monitor/
└── ui/
    ├── app_gui/
    ├── ui_manager_lvgl/
    ├── lvgl_image_handler/
    └── lvgl_sd_fs/
```

The first-level directories group reusable ESP-IDF components by domain.
Component names and public APIs remain independent.

### 6.2 Component Responsibilities

| Component | Responsibility |
|---|---|
| `provisioning_manager` | Temporary BLE provisioning transport lifecycle and cleanup |
| `wifi_manager` | Wi-Fi connection, disconnect, reconnect, events |
| `config_manager` | NVS read/write/erase for credentials and settings |
| `sensor_manager` | Periodic sensor reading and validation |
| `sensor_DHT22` | DHT22-specific sampling driver |
| `ui_manager_lvgl` | LVGL initialization, tick, display integration, and mutex ownership |
| `app_gui` | Queue-driven application screens and status rendering |
| `display_driver` | LCD SPI init, panel driver, LVGL flush integration |
| `waveshare__esp_lcd_st7735` | ST7735 panel implementation used by the display driver |
| `lvgl_image_handler` | Image decoding, scaling, animation, and LVGL image object handling |
| `lvgl_sd_fs` | LVGL file-system bridge for SD-card content |
| `sd_card_manager` | SD-card mount, file access, and diagnostic helpers |
| `firebase_auth` | Firebase sign-in, ID-token validation, caching, and refresh |
| `cloud_manager` | Build telemetry JSON and upload it to Firebase via authenticated HTTPS REST |
| `common` | Shared types, events, error codes, utility macros |
| `performance_monitor` | Runtime CPU, memory, and task-stack diagnostics |

---

## 7. System State Machine

```mermaid
stateDiagram-v2
    [*] --> BOOT
    BOOT --> INIT_DISPLAY
    INIT_DISPLAY --> INIT_LVGL
    INIT_LVGL --> LOAD_CONFIG

    LOAD_CONFIG --> BLE_PROVISIONING: No Wi-Fi config
    LOAD_CONFIG --> WIFI_CONNECTING: Wi-Fi config exists

    BLE_PROVISIONING --> SAVE_CONFIG: Credentials received
    SAVE_CONFIG --> WIFI_CONNECTING

    WIFI_CONNECTING --> RUNNING: Connected
    WIFI_CONNECTING --> WIFI_CONNECT_FAILED: Timeout / failed
    WIFI_CONNECT_FAILED --> BLE_PROVISIONING: User reset / no valid config
    WIFI_CONNECT_FAILED --> WIFI_CONNECTING: Retry

    RUNNING --> CLOUD_SYNCING: New sensor sample
    CLOUD_SYNCING --> RUNNING: Upload success
    CLOUD_SYNCING --> CLOUD_ERROR: Upload failed
    CLOUD_ERROR --> RUNNING: Retry later

    RUNNING --> WIFI_DISCONNECTED: Wi-Fi lost
    WIFI_DISCONNECTED --> WIFI_CONNECTING

    RUNNING --> FACTORY_RESET: Long press button
    FACTORY_RESET --> BLE_PROVISIONING
```

---

## 8. Runtime Task/Event Design

### 8.1 Suggested Tasks

| Task | Priority | Responsibility |
|---|---:|---|
| `app_task` | Medium | Main state machine and event handling |
| `ui_task` | Medium | LVGL tick/handler and screen updates |
| `sensor_task` | Low/Medium | Read sensor periodically |
| `cloud_task` | Low/Medium | Upload data to Firebase |
| `input_task` | Medium | Button debounce and long press |

### 8.2 Event Flow

```text
Sensor Task
    ↓ SENSOR_DATA_READY event
App Controller
    ↓ UI_UPDATE event
UI Task / LVGL

App Controller
    ↓ CLOUD_UPLOAD_REQUEST event
Cloud Task
    ↓ CLOUD_UPLOAD_RESULT event
App Controller
    ↓ UI_UPDATE event
UI Task / LVGL
```

### 8.3 Important Rule

Do **not** update LVGL directly from random tasks or callbacks.

Recommended rule:

```text
Only ui_task updates LVGL objects.
Other tasks send events/messages to ui_task.
```

---

## 9. Roadmap by Sprint

## Sprint 0 — Project Setup

**Goal:** Create clean ESP-IDF project structure.

### Tasks

- [x] Create ESP-IDF project.
- [x] Set target to ESP32-S3.
- [x] Create component folders.
- [x] Add project logging conventions.
- [x] Confirm build/flash/monitor works.
- [x] Create project and component documentation.

### Commands

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

### Done Criteria

- [x] Project builds successfully.
- [x] Firmware boots and prints project name/version.
- [x] Component structure is ready.

### Learning Topics

- ESP-IDF project layout
- Component CMakeLists
- `idf.py` workflow
- Logging basics

---

## Sprint 1 — LCD + LVGL Bring-up

**Goal:** LCD can display a basic LVGL screen.

### Tasks

- [x] Bring up ST7735 LCD using SPI.
- [x] Add LVGL dependency.
- [x] Configure LVGL tick and handler.
- [x] Implement display flush callback.
- [x] Show basic screen with project title.
- [x] Update a counter label periodically during bring-up.

### Example UI

```text
+----------------+
| Smart Gateway  |
| LVGL: OK       |
| Counter: 001   |
+----------------+
```

### Done Criteria

- [x] LCD shows correct colors.
- [x] Text is readable.
- [x] LVGL screen updates without crash.
- [x] No direct LVGL update from non-UI tasks.

### Learning Topics

- SPI LCD
- ST7735 init
- RGB565 color format
- LVGL flush callback
- LVGL draw buffer
- UI task concept

### Common Risks

- Wrong LCD init sequence.
- Wrong color order: RGB/BGR.
- Wrong display offset.
- SPI clock too high.
- LVGL buffer too large or too small.

---

## Sprint 2 — Wi-Fi Station + LVGL Status

**Goal:** Connect ESP32-S3 to Wi-Fi using hardcoded credentials and display status on LCD.

### Tasks

- [x] Implement `wifi_manager`.
- [x] Connect to Wi-Fi with development SSID/password.
- [x] Handle Wi-Fi and DHCP events.
- [x] Display Wi-Fi state on the LVGL screen.
- [x] Display the IP address after connection.
- [ ] Display RSSI. Deferred; RSSI is transported but is not currently shown.

### Example UI

```text
Wi-Fi: Connected
SSID : Home_WiFi
IP   : 192.168.1.50
RSSI : -55 dBm
```

### Done Criteria

- [x] Wi-Fi connects reliably.
- [x] IP is printed in log.
- [x] IP is shown on LCD.
- [x] Disconnect event is detected.

### Learning Topics

- `esp_wifi`
- Wi-Fi station mode
- ESP event loop
- IP event
- Reconnect basics

---

## Sprint 3 — Sensor Manager + UI Update

**Goal:** Read sensor periodically and show values on LCD.

### Tasks

- [x] Implement `sensor_manager`.
- [x] Read DHT22 every 2 seconds.
- [x] Validate sensor value range.
- [x] Send sensor data event to the application layer.
- [x] Update LVGL labels through the GUI queue.

### Example UI

```text
Temp : 28.4 C
Hum  : 70.2 %
Sensor: OK
```

### Done Criteria

- [x] Sensor data is read periodically.
- [x] Invalid readings are handled gracefully.
- [x] UI updates without flicker or crash.
- [x] Sensor task does not call LVGL directly.

### Hardware Acceptance Result

Sensor updates, stale/error handling, LVGL rendering, and task stability were
accepted on the target hardware before Sprint 4 began.

### Learning Topics

- Sensor timing
- Periodic FreeRTOS task
- Queue/event communication
- Data validation

---

## Sprint 4 — Firebase Realtime Database Upload

**Goal:** Upload latest sensor data to Firebase via HTTPS REST API.

### Tasks

- [x] Create Firebase Realtime Database project.
- [x] Define the authenticated latest-value database path.
- [x] Implement `firebase_auth` and `cloud_manager` components.
- [x] Build a bounded JSON telemetry payload.
- [x] Send authenticated HTTPS requests using `esp_http_client`.
- [x] Upload the latest sensor snapshot.
- [x] Display cloud sync/error status on the Sensor LCD screen.

### Suggested Firebase Paths

```text
/devices/esp32s3-001/latest.json
```

Historical storage remains outside Sprint 4; this phase intentionally owns
only the latest-value path.

### Example Payload

```json
{
  "temperature_c": 28.4,
  "humidity_percent": 70.2,
  "sensor_valid": true,
  "sensor_stale": false,
  "sensor_state": 3,
  "last_error": 0,
  "sample_uptime_ms": 3600000,
  "source": "esp32_cloud_manager"
}
```

### Done Criteria

- [x] ESP32 uploads authenticated JSON successfully.
- [x] Firebase shows the latest sensor data at the configured device path.
- [x] Authentication, transport, and HTTP upload failures are detected.
- [x] LCD shows the live Cloud state: `Wait`, `Sync`, `Online`, `Retry`,
  `Auth`, or `Error`.

### Hardware Acceptance Result

Accepted on 2026-07-19:

- [x] Firebase Email/Password authentication succeeds on the ESP32-S3.
- [x] The ESP32-S3 receives a successful Firebase HTTPS response.
- [x] Firebase data changes from firmware telemetry uploads.
- [x] The LCD Cloud indicator follows upload and failure states.
- [x] Failure/retry handling runs without a watchdog reset, crash, or reboot.

### Learning Topics

- HTTPS client
- JSON payload
- TLS/certificate basics
- Cloud upload retry
- Firebase Realtime Database REST API

### Security Notes

- Do not hardcode sensitive production secrets in public GitHub repos.
- For portfolio demo, use restricted test database rules or a disposable Firebase project.
- Firestore direct integration is not recommended for early stage because authentication is more complex.

---

## Sprint 5 — NVS Config Storage

**Goal:** Store Wi-Fi credentials and device settings in NVS.

### Tasks

- [x] Implement `config_manager`.
- [x] Store SSID/password in NVS.
- [x] Load valid persisted config at boot.
- [x] Add schema version policy and explicit legacy migration.
- [x] Add Wi-Fi clear and component factory reset.
- [x] Detect missing/incomplete/invalid/unsupported/legacy config.
- [x] Add optional device identity persistence.
- [x] Add isolated Sprint 5 fault-injection and hardening tests.

### Example Config Fields

```text
wifi_ssid
wifi_password
device_id
upload_interval_sec
config_version
```

### Done Criteria

- [x] Config is saved successfully.
- [ ] Config persists after reboot.
- [x] Missing config is detected.
- [x] Wi-Fi erase works.
- [ ] Updated 38-case Sprint 5 suite passes on ESP32-S3 hardware.
- [ ] Production boot state matrix passes on ESP32-S3 hardware.

### Verification Status

- Production firmware compile/link after hardening: passed with ESP-IDF v6.0.1.
- Renamed 38-case `Test/config_manager` firmware compile/link: passed.
- Historical 14-case Phase 5.3B hardware suite: passed.
- Previous 37-case expanded hardware suite: passed.
- Updated 38-case runtime, reboot persistence, and production boot-state
  tests: pending hardware.

### Learning Topics

- NVS namespace
- Key-value storage
- Config versioning
- Factory reset behavior

---

## Sprint 6 — BLE Wi-Fi Provisioning

**Goal:** Configure Wi-Fi credentials over BLE instead of hardcoding them.

### Recommended Approach

Start with **ESP-IDF Wi-Fi Provisioning Manager** instead of writing custom BLE GATT from scratch.

### Phase 6.1 Status — Complete

- [x] Add the reusable `provisioning_manager` component.
- [x] Initialize the Espressif BLE provisioning scheme.
- [x] Advertise a MAC-derived service using Security 1.
- [x] Verify thread-safe start/stop lifecycle state transitions.
- [x] Stop BLE and de-initialize provisioning resources without deadlock.
- [x] Document the public API, ownership boundaries, and cleanup behavior.

Phase 6.1 is limited to controlled BLE bring-up and cleanup. Credential
reception, validation, NVS handoff, Station connection, and GUI integration
are completed in Phase 6.2.

### Phase 6.2 Status — Complete

- [x] Start provisioning only for `NOT_CONFIGURED`.
- [x] Validate and deep-copy framework-owned credentials.
- [x] Hand credentials to the application only after Wi-Fi success.
- [x] Discard pending credentials after a failed connection attempt.
- [x] Persist credentials only through `config_manager`.
- [x] Verify configuration state and read-back before continuing.
- [x] Recover safely from an injected NVS persistence failure.
- [x] Stop BLE, release provisioning resources, and let `wifi_manager` adopt
  the active Station connection.
- [x] Remove temporary fault-injection code after hardware acceptance.

### Phase 6.3 Status - In Progress

#### Checkpoint 6.3.2 - Dedicated Coordinator Task - Complete

- [x] Run boot policy and bounded provisioning in a dedicated one-shot task.
- [x] Keep `app_main()` responsive while BLE provisioning waits for input.
- [x] Preserve `config_manager`, `provisioning_manager`, and `wifi_manager`
  ownership during credential persistence, cleanup, and connection adoption.
- [x] Expose network readiness only after provisioning adoption completes.
- [x] Defer the 12 KB cloud task until stored connection startup or completed
  provisioning handoff, with retry after temporary allocation pressure.
- [x] Preserve queue-driven Wi-Fi and cloud GUI updates without direct LVGL
  calls from callbacks.
- [x] Verify timeout, reset, reprovision, Wi-Fi connection, Firebase upload,
  and GUI cloud-state recovery on hardware.

Checkpoint 6.3.2 was hardware-accepted by the user on 2026-07-26. Phase 6.3
remains in progress; this checkpoint does not complete its remaining work.

#### Checkpoint 6.3.3 - Runtime Network State Synchronization - Complete

- [x] Forward task-context Wi-Fi manager snapshots into the application
  network coordinator.
- [x] Track runtime `CONNECTING`, `ONLINE`, and `OFFLINE` transitions after
  normal Station ownership begins.
- [x] Ignore transient provisioning Wi-Fi events until persistence, BLE
  cleanup, and connection adoption complete.
- [x] Preserve `wifi_manager` ownership of connection and reconnect behavior.
- [x] Preserve queue-driven GUI updates without calling LVGL from the Wi-Fi
  callback.

Checkpoint 6.3.3 was hardware-accepted by the user on 2026-07-26. Phase 6.3
remains in progress; provisioning-status UI work is still pending.

#### Checkpoint 6.3.4 - Application Service Lifecycle Separation - Implemented

- [x] Initialize Firebase Authentication and the cloud telemetry queue before
  starting the sensor producer.
- [x] Run sensor sampling as a local service independent of network success.
- [x] Schedule network orchestration only after local producers and consumer
  queues are ready.
- [x] Gate the memory-heavy cloud task on coordinator `CONNECTING` or `ONLINE`
  states that cannot overlap active BLE provisioning.
- [x] Recover a `GOT_IP` event near the provisioning deadline through one
  bounded handoff grace before declaring the coordinator failed.
- [x] Keep sensor, cloud, Wi-Fi, provisioning, GUI, and coordinator ownership
  within their existing components.
- [x] Build and statically review callback, queue, critical-section, and
  credential-handling paths.
- [ ] Confirm provisioning, late-DHCP recovery, GUI/sensor responsiveness,
  reconnect, watchdog, and Firebase recovery behavior on hardware.

Checkpoint 6.3.4 is implemented and statically verified. Hardware acceptance
is not yet claimed, and Phase 6.3 remains in progress.

### Phase 6.4.3 Status - Implemented / Hardware Test Pending

- [x] Build QR JSON from the exact active BLE service using Espressif's
  `v1`, Security 1, and `ble` schema.
- [x] Configure BT/NimBLE, Security 1, and LVGL QR support through
  `sdkconfig.defaults`.
- [x] Match the official `wifi_prov` example's five framework Wi-Fi
  connection attempts.
- [x] Copy the QR payload through coordinator and GUI-owned queues without
  logging PoP or credentials.
- [x] Render the QR with LVGL at the largest integer module scale that fits
  the 160x128 screen and a standards-compliant quiet zone.
- [x] Preserve provisioning, Wi-Fi manager, config manager, and LVGL ownership.
- [x] Confirm QR scanning and BLE Wi-Fi credential handoff on hardware.

The user confirmed QR scanning and a successful provisioned Wi-Fi connection
on target hardware. Cross-phase GUI, retry, cloud, and endurance acceptance
remain covered by the final A-N matrix.

### Phase 6.4.4 Status - Implemented / Hardware Test Pending

- [x] Publish copied, non-sensitive provisioning manager progress outside its
  lifecycle critical section.
- [x] Bridge the existing single Wi-Fi callback into real association, DHCP,
  and disconnect provisioning states without promoting application `ONLINE`.
- [x] Deliver provisioning status through a dedicated length-one overwrite
  queue rather than the screen-command queue.
- [x] Make the GUI QR cache session-owned and clear it only through explicit
  session invalidation.
- [x] Keep ordinary credential failure non-terminal so the same BLE session
  can accept another phone submission.
- [x] Map verified persistence, cleanup, and adoption to
  `SAVING_CONFIG -> CLEANING_UP -> SUCCESS`.
- [x] Hold `SUCCESS` for 1500 ms, request `WIFI_STATUS`, and preserve the
  existing timeout route to `SENSOR_DASHBOARD`.
- [x] Keep automatic `RETRYING` and session restart outside this checkpoint.
- [ ] Confirm progress ordering, wrong-password retry within the same BLE
  session, timeout cleanup, success dwell, and final routing on hardware.

Phase 6.4.4 is implemented and build-verified. Hardware acceptance remains
pending; automatic retry/restart was deferred to Phase 6.4.5 at that
checkpoint and is now implemented. Final Phase 6.4 hardware acceptance remains
pending.

### Phase 6.4.5 Status - Implemented / Hardware Test Pending

- [x] Add a three-session default retry envelope with 1000 ms failure dwell
  and 1500 ms retry backoff.
- [x] Reinitialize `provisioning_manager` only after clean `STOPPED` and reuse
  and reset its single credential queue.
- [x] Keep wrong-password recovery inside the same BLE session without
  consuming the outer session budget.
- [x] Classify retryable timeout/session failure separately from storage,
  adoption, and internal failures.
- [x] Attach a non-zero generation to manager progress, GUI status, QR update,
  and QR clear messages; reject stale generations.
- [x] Show the active one-based provisioning session and configured maximum as
  `Session n/max` on the QR screen without reducing QR size.
- [x] Retain BLE controller memory between retries and release it when the
  bounded envelope terminates.
- [x] Preserve coordinator `PROVISIONING` and cloud gating during retry; keep
  final exhaustion on the Provisioning Screen without reboot.
- [x] Preserve the successful Phase 6.4.4 cleanup, adoption, success dwell,
  and final screen routing.
- [ ] Confirm timeout retry, same-session password correction, exhaustion,
  second-session success, stale-event rejection, injected failures, and
  30-60-minute resource stability on hardware.

Phase 6.4.5 is implemented and statically build-verified. Hardware acceptance
remains part of the final A-N matrix.

### Phase 6.4.6 Status - Implemented / Hardware Test Pending

- [x] Feed the cloud manager from the existing Wi-Fi callback through a
  non-blocking IPv4 state and non-zero network epoch.
- [x] Replace blind cloud retry delays with task-notification waits that wake
  for network edges and telemetry without allowing telemetry hot loops.
- [x] Reset the single task-owned HTTP client on network/token generation,
  transport, HTTP 0/401/403/408/429/5xx, and terminal request failures.
- [x] Classify cloud attempts explicitly instead of treating every HTTP status
  zero as retryable.
- [x] Add bounded HTTP 401 recovery and at most one forced token recovery for a
  persistent HTTP 403 rejection.
- [x] Split Firebase Authentication operation serialization from short
  token/status state protection and expose observable token invalidation.
- [x] Preserve the latest-value telemetry queue across retries and the
  new-sample-during-request race.
- [x] Correct the ESP32-S3/NimBLE terminal release to
  `esp_bt_mem_release(ESP_BT_MODE_BLE)` after clean `STOPPED`.
- [x] Make BLE memory reclamation best-effort so it cannot replace successful
  adoption or an existing provisioning failure.
- [x] Preserve one cloud task, one Wi-Fi callback, provisioning/cloud gating,
  certificate verification, Firebase endpoint, and GUI mappings.
- [ ] Confirm normal upload, reconnect-during-backoff, transport and HTTP
  recovery, 401/403 policy, latest telemetry, provisioning regression, token
  refresh, and two-hour resource stability on hardware.

Phase 6.4.6 is implemented and statically build-verified. Hardware acceptance
remains part of the final A-N matrix.

### Phase 6.4.7 Status - Implemented / Hardware Regression Pending

- [x] Verify the Phase 6.4.1-6.4.6 prerequisite architecture and component
  ownership.
- [x] Review queues, callbacks, tasks, mutexes, generations, cleanup, boot
  policy, provisioning retry, cloud recovery, and cloud-start gating.
- [x] Remove stale closure documentation and confirm no temporary production
  fault driver remains.
- [x] Preserve the original BLE service-start error when subsequent framework
  cleanup also fails, while keeping the manager in `FAILED`.
- [x] Clean partial `ui_manager_lvgl` initialization resources and allow a
  safe same-boot retry after initialization failure.
- [x] Document the final ownership model, known limitations, and A-N hardware
  regression matrix.
- [ ] Execute the final A-N matrix on the ESP32-S3 target.

**Phase 6.4.7 - IMPLEMENTED / HARDWARE REGRESSION PENDING**

**Phase 6.4 - IMPLEMENTED / FINAL HARDWARE ACCEPTANCE PENDING**

Build and static checks establish review readiness but do not prove BLE,
Wi-Fi, LCD, TLS, Firebase, heap, or endurance behavior on the target. Only the
matrix below can close the remaining hardware acceptance.

### Phase 6.4 Final Hardware Acceptance Matrix

Use a serial monitor at the default project log level. Do not enable logging
that prints provisioning session material. Record the firmware revision,
board, power source, AP/hotspot, phone provisioning application, and duration.

#### A. Configured-device boot

1. Reboot with a valid stored configuration.
2. Verify `BOOT -> WIFI_STATUS -> SENSOR_DASHBOARD -> Cloud Online`.
3. Verify BLE provisioning does not start.

#### B. First-session provisioning success

1. Erase only the application Wi-Fi configuration through an approved test
   procedure.
2. Verify direct `PROVISIONING` display with no `BOOT` flash and a scannable
   QR.
3. Submit correct credentials.
4. Verify `SUCCESS -> WIFI_STATUS -> SENSOR_DASHBOARD -> Cloud Online`.

#### C. Wrong password then correct password

1. Submit a wrong password and verify `FAILED`.
2. Verify the same BLE session, QR, and `Session 1/3` remain active with no
   `RETRYING`.
3. Submit the correct password and verify the normal success flow.

#### D. Timeout and replacement session

1. Let session 1 expire.
2. Verify `TIMEOUT -> CLEANING_UP -> RETRYING`.
3. Verify session 2 starts only after manager `STOPPED`, shows a current QR,
   and accepts correct credentials to complete successfully.

#### E. Retry exhaustion

1. Let every configured session expire.
2. Verify exactly the configured number of sessions, final `TIMEOUT` or
   `FAILED`, hidden/invalidated QR, no extra session, no reboot, and no cloud
   start.

#### F. Reboot persistence

1. Provision successfully, then power-cycle the board.
2. Verify stored-config boot and no provisioning screen or BLE service.

#### G. Runtime Wi-Fi disconnect

1. During configured runtime, turn the AP/hotspot off and then on.
2. Verify `wifi_manager` reconnect backoff restores Wi-Fi.
3. Verify no provisioning screen, config erase, or coordinator provisioning
   restart occurs.

#### H. Cloud network recovery

1. Begin at `Cloud Online`, turn the AP/hotspot off, and verify `Cloud Wait`.
2. Restore the AP/hotspot and verify `Cloud Sync -> Cloud Online`.

#### I. Internet-only failure

1. Keep Wi-Fi associated with a valid IPv4 address while removing Internet
   access.
2. Verify `Cloud Retry` with bounded backoff.
3. Restore Internet and verify `Cloud Online`.

#### J. Cloud wake during long backoff

1. Allow cloud retry to reach a long delay.
2. Recover the network before the deadline.
3. Verify prompt task wake, network-epoch client reset, newest telemetry
   upload, and `Cloud Online`.

#### K. Retryable HTTP or transport fault

1. Use a controlled external failure that does not require production
   fault-injection code.
2. Verify transport/TLS/DNS failure or HTTP 408/429/5xx produces bounded
   `Retry`, a clean client reset, and recovery to `Online`.

#### L. Authentication recovery

1. Reject the active token and verify bounded invalidation, token recovery,
   and return to `Online`.
2. Sustain an authorization rejection and verify terminal `Auth Error`
   without a hot sign-in loop.

#### M. Latest telemetry

1. Produce several sensor samples during an outage and active retry.
2. Restore service and verify the newest pending sample reaches Firebase;
   historical samples are not expected.

#### N. Endurance

Run for at least 2 hours; 4-8 hours is recommended. During the run:

- repeat Wi-Fi disconnect/reconnect;
- repeat an Internet-only outage;
- keep sensor updates active;
- exercise cloud retry and recovery;
- exercise screen transitions;
- include at least one provisioning retry when practical.

Acceptance for A-N requires:

- no Guru Meditation, watchdog, stack overflow, or LVGL assertion;
- no continuous heap-loss trend;
- no duplicate task or callback;
- no stale screen object or provisioning-session collision;
- no cloud-client leak;
- no sensitive serial output.

### Phase 6.4 Known Limitations

- No manual SSID/password entry UI or touch retry button.
- No runtime user-triggered reprovisioning or factory-reset UI.
- Proof of Possession is a static development value; there is no
  device-specific protected manufacturing flow.
- Firebase device credentials still depend on the current development
  configuration model and must move to protected local configuration before
  production/publication.
- Telemetry is latest-value only; there is no offline history or SD-backed
  cloud queue.
- The one-shot cloud service has no public stop/deinit or operator restart API;
  terminal `AUTH_ERROR` and deterministic `ERROR` remain latched.
- MQTT, OTA, cloud command reception, a custom mobile app, and dashboard
  redesign remain out of scope.

### Tasks

- [x] Add BLE provisioning component.
- [x] Start provisioning if no Wi-Fi config exists.
- [x] Send SSID/password from phone/tool.
- [x] Save credentials to NVS.
- [x] Stop BLE after provisioning.
- [x] Connect Wi-Fi using provisioned credentials.
- [x] Show provisioning status on LVGL.

### Example UI

```text
Mode: BLE Provisioning
Device: SmartGW_001
Status: Waiting for Wi-Fi config
```

### Done Criteria

- [x] Device advertises BLE provisioning service.
- [x] Phone/tool can provision Wi-Fi.
- [x] Credentials are saved and read back.
- [x] Device connects to Wi-Fi after provisioning.
- [x] BLE is stopped after provisioning.

### Learning Topics

- BLE provisioning
- Wi-Fi provisioning manager
- BLE vs Wi-Fi coexistence
- Provisioning state flow

### Common Risks

- BLE memory usage.
- Wi-Fi/BLE coexistence behavior.
- Mobile tool/app friction.
- Credentials not saved correctly.

---

## Sprint 7 — Button Factory Reset + Recovery

**Goal:** Add a physical recovery path.

### Tasks

- [ ] Add button input.
- [ ] Implement debounce.
- [ ] Detect long press, for example 5 seconds.
- [ ] Erase Wi-Fi config from NVS.
- [ ] Restart provisioning mode.
- [ ] Display reset confirmation/status on LVGL.

### Done Criteria

- [ ] Short press does not erase config accidentally.
- [ ] Long press reliably resets config.
- [ ] Device returns to provisioning mode.
- [ ] User can recover from wrong Wi-Fi credentials.

### Learning Topics

- GPIO input
- Debounce
- Long press logic
- Safe factory reset flow

---

## Sprint 8 — Reconnect + Cloud Retry

**Goal:** Make the device robust when network/cloud is unstable.

### Tasks

- [x] Implement Wi-Fi reconnect backoff.
- [x] Detect disconnected state.
- [x] Pause Firebase upload when offline.
- [x] Add retry count.
- [x] Add simple cloud queue or latest-only retry.
- [x] Show offline/cloud error state on LVGL.

### Done Criteria

- [ ] Device recovers when Wi-Fi router is turned off/on.
- [ ] Device does not crash when Firebase is unreachable.
- [ ] UI clearly shows offline/error state.
- [ ] Logs are useful for debugging.

### Learning Topics

- Reconnect strategy
- Timeout handling
- Retry/backoff
- Error state design

---

## Sprint 9 — Polish for Portfolio

**Goal:** Make the project presentable for GitHub/CV/interview.

### Tasks

- [ ] Write clean `README.md`.
- [ ] Add architecture diagram.
- [ ] Add state machine diagram.
- [ ] Add setup instructions.
- [ ] Add demo screenshots/photos.
- [ ] Record short demo video.
- [ ] Add known issues section.
- [ ] Add future improvements section.

### Done Criteria

- [ ] Another developer can understand the project from README.
- [ ] Build and setup steps are clear.
- [ ] Architecture is explainable in interview.
- [ ] Project has a clear demo path.

### Learning Topics

- Technical documentation
- Embedded portfolio presentation
- Interview storytelling

---

## 10. Optional Future Features

| Feature | Value | Risk | Recommendation |
|---|---|---:|---|
| MQTT | More IoT-standard | Medium | Add after Firebase works |
| OTA | Very valuable | High | Add near the end |
| Local web dashboard | Useful fallback | Medium | Optional, not MVP |
| Firestore | More scalable cloud DB | High | Avoid early |
| Custom mobile app | Better UX | Very high | Avoid unless needed |
| BLE custom GATT | Deep BLE learning | Medium-high | Do after provisioning manager |
| Sensor history chart on LVGL | Nice UI | Medium | Add if RAM allows |
| Time sync using SNTP | Useful timestamp | Low-medium | Add before historical logging |

---

## 11. Learning Map

| Project Feature | ESP-IDF / Embedded Topics |
|---|---|
| LCD + LVGL | SPI, LCD driver, RGB565, flush callback, UI task |
| Wi-Fi | `esp_wifi`, event loop, IP event, reconnect |
| BLE provisioning | BLE, GATT concept, provisioning manager, coexistence |
| Firebase upload | HTTPS, `esp_http_client`, TLS, JSON, retry |
| NVS | flash key-value storage, config versioning |
| Sensor | GPIO/timing, periodic task, validation |
| Button reset | interrupt/polling, debounce, long press |
| App controller | state machine, event-driven design |
| Robustness | timeout, retry, watchdog later |
| Documentation | README, diagrams, demo, interview explanation |

---

## 12. Definition of Done — Project Level

The project is considered portfolio-ready when:

- [ ] Device can be provisioned through BLE.
- [ ] Device connects to Wi-Fi without hardcoded credentials.
- [ ] LCD shows meaningful LVGL UI.
- [ ] Sensor data is displayed locally.
- [ ] Sensor data is uploaded to Firebase.
- [ ] Wi-Fi config survives reboot.
- [ ] Button factory reset works.
- [ ] Device can recover from Wi-Fi disconnect.
- [ ] Code is split into clear components.
- [ ] README explains setup, architecture, and demo.
- [ ] There is at least one demo video or photo set.

---

## 13. Risk Register

| Risk | Impact | Probability | Mitigation |
|---|---:|---:|---|
| LCD ST7735 init/color issue | Medium | High | Start with LCD-only test project |
| LVGL memory issue | Medium | Medium | Use small draw buffer, simple UI |
| BLE provisioning setup friction | High | Medium | Use ESP-IDF provisioning manager first |
| Wi-Fi/BLE coexistence issue | Medium | Medium | Stop BLE after provisioning |
| Firebase HTTPS/auth issue | High | Medium | Start with Realtime Database REST and test project |
| Task/thread-safety bug with LVGL | High | Medium | Only UI task touches LVGL |
| Scope creep | High | High | Follow sprint order and avoid optional features early |

---

## 14. Development Log Template

Use this section to track daily/weekly progress.

### Log Entry Template

```markdown
## YYYY-MM-DD — <Short title>

### Goal

### What I did

### Result

### Bugs / Issues

### Root cause

### Fix

### What I learned

### Next step
```

---

## 15. Sprint Tracking Board

| Sprint | Name | Status | Start Date | End Date | Notes |
|---:|---|---|---|---|---|
| 0 | Project setup | Done |  |  | Project structure and ESP-IDF workflow established. |
| 1 | LCD + LVGL bring-up | Done |  |  | ST7735 and LVGL hardware-accepted. |
| 2 | Wi-Fi + LVGL status | Done |  |  | Station events, DHCP status, and LCD UI accepted; RSSI display deferred. |
| 3 | Sensor + UI update | Done |  |  | Sensor queue, stale/error behavior, and LCD updates hardware-accepted. |
| 4 | Firebase upload | Done |  | 2026-07-19 | Hardware upload, Firebase data, failure handling, and LCD Cloud status accepted. |
| 5 | NVS config storage | Done |  | 2026-07-26 | Persistence, integrity, migration, and recovery tests accepted. |
| 6 | BLE provisioning | In progress |  |  | Phase 6.4 implemented; final A-N hardware acceptance pending. |
| 7 | Factory reset | Not started |  |  |  |
| 8 | Reconnect + retry | In progress |  |  | Wi-Fi/cloud recovery implemented; final target-hardware recovery and endurance acceptance pending. |
| 9 | Portfolio polish | Not started |  |  |  |

---

## 16. Decision Log

| Date | Decision | Reason | Revisit? |
|---|---|---|---|
| 2026-06-30 | Use project-driven learning instead of learning all ESP32 topics upfront | More practical, less boring, better retention | No |
| 2026-06-30 | Use BLE for Wi-Fi provisioning and Wi-Fi for main operation | Common real-world IoT flow | No |
| 2026-06-30 | Use LVGL for LCD UI | Better local UI structure and portfolio value | No |
| 2026-06-30 | Use Firebase Realtime Database first | Simpler REST flow than Firestore | Yes, after MVP |
| 2026-06-30 | Avoid custom mobile app early | Prevent scope creep | Yes, after MVP |
| 2026-06-30 | Stop BLE after provisioning | Reduce RAM/radio coexistence complexity | No |
| 2026-07-26 | Run boot provisioning in a dedicated coordinator task and defer cloud task allocation until network handoff | Keep `app_main()` responsive and avoid BLE/cloud internal-RAM contention | No |

---

## 17. Interview/CV Talking Points

After finishing MVP, the project can be described as:

> I built an ESP32-S3 IoT gateway that uses BLE for Wi-Fi provisioning, Wi-Fi for internet connectivity, LVGL for a local LCD dashboard, and Firebase Realtime Database for cloud data logging. The firmware is structured with ESP-IDF components, FreeRTOS tasks, event-driven communication, NVS-based configuration storage, and a state machine for provisioning, connection, running, and recovery states.

Key points to explain in interview:

- Why BLE provisioning is useful.
- Why BLE is stopped after Wi-Fi provisioning.
- How Wi-Fi reconnect is handled.
- How LVGL is updated safely from a UI task.
- How NVS stores configuration.
- How Firebase upload handles failure/retry.
- How the project is split into components.
- What bugs were encountered and how they were debugged.

---

## 18. Immediate Next Step

Start with:

```text
Sprint 0: Project setup
Sprint 1: LCD + LVGL bring-up
```

Do **not** start from BLE or Firebase first.

Reason:

```text
LCD + LVGL gives fast visual feedback.
Wi-Fi/Firebase/BLE can be added after the local UI foundation is stable.
```

---

## 19. Current Project State

```text
Current phase: Planning
Current focus: Prepare roadmap and tracking document
Next action: Create ESP-IDF project skeleton and bring up LCD + LVGL
Main risk: Scope creep from adding too many cloud/network features too early
Recommended discipline: Finish one sprint at a time before adding optional features
```
