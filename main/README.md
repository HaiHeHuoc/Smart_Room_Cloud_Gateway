# Main Application Notes

## Purpose

`main.c` is the current composition root. It initializes platform services and
components, maps Wi-Fi and sensor manager snapshots into GUI types, and keeps a
small diagnostic loop running.

## Current Startup Order

1. Log project identity and optionally start `performance_monitor`.
2. Initialize NVS, ESP-NETIF, and the default ESP event loop.
3. Initialize the LCD display driver.
4. Initialize LVGL and display integration.
5. Initialize the SD card and register the LVGL `S:` filesystem.
6. Initialize `app_gui`, start its UI task, and create the Wi-Fi screen.
7. Initialize `wifi_manager`, register its callback, and start connection.
8. Initialize `sensor_manager`, register its callback, and start sampling.
9. Read and log a sensor status snapshot every five seconds.

## Event Flow

```text
wifi_manager callback
    -> map to ui_wifi_status_t
    -> app_gui_post_wifi_status()

sensor_manager callback
    -> map to ui_sensor_status_t
    -> app_gui_post_sensor_status()
```

The callbacks do not access LVGL. Screen updates are consumed by the
application GUI task.

## Current Configuration

- `PERFORMANCE_MONITOR` is disabled at compile time.
- Sensor sampling uses a 2500 ms period and a 10000 ms stale timeout.
- Wi-Fi credentials are currently development values supplied directly in
  `main.c`; move them to provisioning or local build configuration before
  publishing firmware.
- The main loop emits temperature and humidity diagnostics every five seconds.

## Important Notes

- `network_platform_init()` owns the one-time NVS, ESP-NETIF, and default event
  loop setup required by `wifi_manager`.
- The display handle has static lifetime because `ui_manager_lvgl` borrows it.
- GUI callbacks copy their inputs before returning and do not retain manager
  snapshot pointers.
- The current source contains development diagnostics and demo hooks that are
  intentionally left unchanged.
- There is no coordinated shutdown path because the firmware currently runs
  continuously after startup.

## Build

From the project root in an ESP-IDF environment:

```powershell
idf.py build
```

Flash and monitor after selecting the correct serial port:

```powershell
idf.py -p <PORT> flash monitor
```

## Future Attention

- Move development Wi-Fi credentials out of source code.
- Replace the main-loop sensor warning log with the final application control
  flow when Phase 3 is complete.
- Add explicit recovery policy for component initialization failures.
