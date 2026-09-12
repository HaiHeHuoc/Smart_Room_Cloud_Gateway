# Phase 17 — Xiaozhi Read-Only MCP Tools

Status: **IN PROGRESS**

- Sensor answer: **IMPLEMENTED / BUILD VERIFIED / HIL ACCEPTED**
- Cloud-sync status: **IMPLEMENTED / BUILD VERIFIED / HIL ACCEPTED BY USER**
- System status: **IMPLEMENTED / BUILD VERIFIED / HIL PENDING**

## Goal

When a user asks Xiaozhi for the current room temperature or humidity, Xiaozhi
must be able to retrieve the current Smart Room sensor reading through the
existing Xiaozhi WebSocket session and answer from that reading.

## Read-only MCP contract

The device registers one dedicated MCP tool:

```text
smart_room.get_current_temperature_humidity
```

The tool description explicitly applies to questions about room temperature,
humidity, climate, and current Smart Room conditions.  It returns both a
structured result and a short, unambiguous text result containing
`temperature_c` and `humidity_percent`.

The tool has no input and has no device-side state-changing operation.

## Data source and validity

The composition root reads the public `sensor_manager` status only.  It
exposes the last valid temperature and humidity sample only when the sample is
not stale.  A transient failed DHT read therefore cannot turn into a reported
`-1.0` room reading.

`xiaozhi_foundation` owns MCP registration and callback lifetime; it receives
only a bounded snapshot provider and does not expose Xiaozhi or MCP handles to
the sensor component.

## HIL acceptance

1. Flash this branch and wait for a valid, non-stale DHT sample.
2. Ask: `Nhiệt độ trong phòng của tôi hiện tại là bao nhiêu?`
3. Confirm the serial log contains `Smart Room sensor MCP called; data=available`.
4. Confirm Xiaozhi speaks the temperature returned by the tool, rather than
   saying that it lacks information.

The remote Xiaozhi agent still decides when to invoke a registered tool.  A
build or a visible Smart Room capability is not an end-to-end PASS until the
callback marker and spoken answer are both observed in the same test.

## Accepted evidence

- Build passed with the production sensor-query tool enabled.
- Firmware was flashed to COM4 and its image hash was verified by the flasher.
- The user confirmed three voice-query test cases returned the correct current
  room temperature and humidity through Xiaozhi.

## Cloud-sync status slice

The device also registers the no-argument, read-only MCP tool:

```text
smart_room.get_cloud_sync_status
```

It answers questions about whether the Gateway's cloud uploader has recently
succeeded, is uploading, is retrying, or has a classified failure. The result
contains only normalized uploader state, age of the most recent successful
upload when available, consecutive failure count, failure class, and configured
retry delay when a retry is active. It deliberately does **not** expose
Firebase URLs, HTTP bodies, credentials, tokens, identifiers, or configuration.

The tool does not prove Internet reachability and does not claim that a newer
sensor sample has already uploaded. `main` copies the public
`cloud_manager_get_status()` snapshot into the foundation provider; the
foundation owns MCP registration and session lifetime without depending on
`cloud_manager`.

### Accepted HIL evidence

The user confirmed that the cloud-sync voice path works on target hardware.
This establishes user-observed end-to-end behavior for the slice. No additional
serial log, upload timestamp, retry case, or failure case is asserted by this
checkpoint.

Build success proves registration and linkage only. The subsequent user
confirmation establishes the voice-path acceptance; detailed runtime artifacts
were not captured here.

## System-status slice

The device also registers the no-argument, read-only MCP tool:

```text
smart_room.get_system_status
```

It answers broad local-health questions such as “Thiết bị đang hoạt động bình
thường không?” by returning only normalized states for the sensor, cloud
uploader, SNTP time, SD recovery service, and audio manager. `ready`,
`synced`, `idle`, and `busy` are non-error states; `busy` is expected during a
voice interaction. `attention` reports a component with a non-normal state,
and `unavailable` means its public snapshot could not be read.

`main` reads only public `*_get_status()` APIs and converts their snapshots to
bounded identifier tokens. The foundation receives those tokens through a
provider, validates them before JSON formatting, and owns MCP attachment and
session lifetime. The tool does not expose SSID, IP address, Wi-Fi status,
Internet reachability, endpoints, credentials, tokens, files, or configuration.
Its successful invocation cannot prove Internet reachability because Xiaozhi
must already have a live session to call it.

### HIL acceptance still required

1. Flash this branch after the build gate passes.
2. Ask a natural question such as `Thiết bị đang hoạt động bình thường không?`.
3. Confirm `Smart Room system-status MCP called; data=available` in serial output.
4. Confirm the spoken response accurately distinguishes normal `busy` audio
   activity from an `attention` or `unavailable` component state.

## Git safety

The tracked `app_common.h` file is restored to its branch-base configuration;
Firebase authentication information is not part of this Phase-17 change set.
