# `provisioning_manager` Component

## Purpose

`provisioning_manager` owns temporary BLE Wi-Fi provisioning. It wraps
Espressif `network_provisioning`, uses BLE Security 1, deep-copies received
credentials, publishes non-sensitive progress, supports bounded same-boot
replacement sessions, and releases BLE resources after the retry envelope.

It does not own persistent storage, normal Wi-Fi reconnect, cloud upload, GUI
rendering, or application reboot policy.

## Version 1 Status

```text
Release: v1.0.0
Status: Implemented and hardware accepted
```

## Implemented Behavior

- BLE transport through Espressif Network Provisioning Manager.
- Security 1 with a development Proof of Possession.
- Unique service name derived from the final Station MAC bytes.
- Espressif-compatible `v1` / `ble` QR payload.
- Five framework connection attempts per received credential set.
- Bounded QR-payload copy while the matching session is active.
- Deep-copy validation and zeroization of Wi-Fi credentials.
- Length-one verified credential handoff queue.
- Non-sensitive, generation-aware progress callback.
- Clean `STOPPED -> READY` reinitialization for replacement sessions.
- Exact generation filtering for stale asynchronous events.
- Bounded late-DHCP handoff validation using matching SSID and IPv4.
- Asynchronous deinitialization to avoid framework callback deadlock.
- Explicit terminal BLE memory release after the retry envelope.
- No intentional password or Proof-of-Possession logging.

## Public API

| API | Responsibility |
|---|---|
| `provisioning_manager_register_progress_callback()` | Register one copied progress callback |
| `provisioning_manager_init(generation)` | Initialize or cleanly reinitialize one generation |
| `provisioning_manager_start()` | Start BLE advertising and provisioning |
| `provisioning_manager_get_qr_payload()` | Copy the exact active generation's QR JSON |
| `provisioning_manager_stop()` | Begin asynchronous shutdown |
| `provisioning_manager_release_ble_memory()` | Permanently release BLE memory after final use |
| `provisioning_manager_get_state()` | Copy lifecycle state |
| `provisioning_manager_is_wifi_handoff_pending()` | Report pending framework connection/handoff |
| `provisioning_manager_arm_late_wifi_handoff()` | Bind one pending copy to a generation |
| `provisioning_manager_confirm_late_wifi_handoff()` | Verify matching active SSID and promote the copy |
| `provisioning_manager_discard_late_wifi_handoff()` | Zeroize an unverified retained copy |
| `provisioning_manager_receive_wifi_credentials()` | Receive a verified caller-owned credential copy |

Generation zero is reserved. QR and credential APIs return caller-owned copies,
not manager-owned pointers.

## Lifecycle

```text
UNINITIALIZED
    -> READY
    -> STARTING
    -> ACTIVE
    -> STOPPING
    -> STOPPED
    -> READY for the next non-zero generation

any initialization/start/cleanup failure
    -> FAILED
```

`STOPPED` is a clean barrier. A failed session is not silently reused.

## Credential Handoff

```text
credentials received
    -> validate and deep-copy
    -> framework attempts Wi-Fi connection

framework failure
    -> zeroize pending copy

framework success
    -> move independent copy to length-one queue
    -> application persists through config_manager
    -> application adopts Station through wifi_manager

grace expires while connection is still pending
    -> bind copy to the current generation
    -> stop and deinitialize BLE
    -> allow bounded DHCP settle
    -> require matching SSID plus valid IPv4
    -> promote or zeroize the copy
```

The caller must clear its credential copy after persistence or error handling.
Progress snapshots never contain SSID, password, QR JSON, or internal pointers.

## Threading And Cleanup

- Short lifecycle state and credential-copy operations use a critical section.
- BLE, Wi-Fi, logging, queue, task, and callback operations execute outside the
  critical section.
- Progress callbacks run in normal task context and must return quickly.
- Framework deinitialization runs in a one-shot cleanup task because direct
  deinitialization from the framework end callback can deadlock.
- The retained queue is overwritten with a zero item before reset because queue
  reset alone does not guarantee erasure of backing storage.
- Active service identity and QR payload are cleared when their session ends.

## Development Proof Of Possession

Version 1 currently contains a fixed development Proof of Possession in the
component source. It is deliberately documented as a development value, not a
production secret.

Publishing the repository reveals this value. That is acceptable only for the
portfolio/development threat model where:

- provisioning is temporary;
- the device is under owner control;
- BLE is stopped after provisioning;
- no claim of unique per-device provisioning security is made.

Before a production deployment, replace it with a unique per-device value
provisioned during manufacturing or loaded from protected configuration. Do not
publish readable provisioning QR codes because they also contain the active
Proof of Possession.

## Security Contract

- Never log Wi-Fi passwords, QR payloads, or the Proof of Possession.
- Zeroize caller and manager credential copies after ownership ends.
- Do not leave BLE provisioning active during normal operation.
- Do not use the development PoP as a production authentication factor.
- Do not publish screenshots/video containing a reusable QR payload.
- Preserve generation checks so stale session events cannot persist or adopt
  credentials.
- Factory reset must quiesce provisioning before persistent erasure.

See [`SECURITY.md`](../../../../SECURITY.md) for public-release controls.

## Application Integration

`app_network_coordinator` owns:

- deciding when provisioning is required;
- session timeout, retry count, and backoff;
- persisting verified credentials;
- Wi-Fi adoption and final screen routing;
- terminal BLE memory-release timing.

`provisioning_manager` remains a transport/lifecycle component and does not
call `config_manager`, `wifi_manager`, LVGL, cloud, or reboot APIs.

## Future Attention

- Move the Proof of Possession to protected per-device configuration.
- Add manufacturing identity and revocation policy.
- Consider Security 2 only after device identity, key provisioning, and product
  lifecycle requirements are defined.
- Add a public stop/deinit contract only when runtime reprovisioning is required.
