# Phase 17 — Xiaozhi Authoritative Sensor Answer

Status: **IMPLEMENTED / BUILD VERIFIED / HIL ACCEPTED / GIT-READY**

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

## Git safety

The tracked `app_common.h` file was restored to its branch-base version before
this checkpoint was prepared.  Firebase authentication information is not part
of this Phase-17 change set.
