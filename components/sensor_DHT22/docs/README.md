# sensor_DHT22 Component Notes

## Purpose

`sensor_DHT22` is the hardware-facing DHT22 wrapper. It reads one temperature
and humidity sample from the pin configured in `board_config.h`, validates the
values, and returns a small application-owned data structure.

## What Is Implemented

- Uses the external `dht` component with the AM2301/DHT22 device type.
- Reads relative humidity and temperature as floating-point values.
- Rejects non-finite values.
- Enforces the DHT22 operating ranges used by this project:
  temperature from -40 to 80 degrees Celsius and humidity from 0 to 100%.
- Copies output data only after the complete sample passes validation.
- Includes an optional periodic bring-up task for standalone hardware testing.

## Public API

| API | Current role |
| --- | --- |
| `dht22_sensor_read()` | Read and validate one sample from the configured DHT22. |
| `dht22_bringup_start()` | Start the standalone periodic sensor logging task. |

## Basic Usage

```c
dht22_sensor_data_t sample = {0};

esp_err_t ret = dht22_sensor_read(&sample);
if (ret == ESP_OK) {
    /* sample.temperature_c and sample.humidity_percent are valid. */
}
```

Normal application code should use `sensor_manager` instead of repeatedly
calling this wrapper itself. The manager owns periodic scheduling, stale-data
state, statistics, and GUI notification.

## Important Notes

- DHT22 requires a relatively slow sampling period. The current manager uses
  2500 ms and rejects configurations below 2000 ms.
- The sensor pin comes from `DHT22_PIN_GPIO` in `board_config.h`.
- A communication failure is returned from the underlying DHT driver.
- A finite but out-of-range sample returns `ESP_ERR_INVALID_RESPONSE`.
- Do not run `dht22_bringup_start()` together with `sensor_manager`; both would
  access the same one-wire-style sensor timing path.
- The bring-up start helper does not report `xTaskCreate()` failure because its
  current public return type is `void`.

## Future Attention

- Return task-creation status if the bring-up helper remains part of the
  supported API.
- Add a deinit/power-control path only if the hardware design needs it.
- Keep scheduling and retry policy in `sensor_manager`, not in this wrapper.
