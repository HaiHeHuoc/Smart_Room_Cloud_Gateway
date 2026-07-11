# ui_manager_lvgl Component Notes

## Purpose

`ui_manager_lvgl` owns LVGL setup and LVGL-safe UI execution.
It creates the LVGL display, allocates draw buffers, connects LVGL flush to the
LCD display driver, starts the LVGL tick timer, and provides task helpers for
calling `lv_timer_handler()`.

Application code should avoid calling LVGL directly from random tasks. UI work
should be controlled through this component or protected by its LVGL mutex.

## What Is Done

- Initializes LVGL core.
- Allocates DMA-capable LVGL draw buffer.
- Allocates a rotation buffer when landscape mode is enabled.
- Creates an LVGL display using `LCD_H_RES` and `LCD_V_RES`.
- Sets LVGL color format to RGB565.
- Supports landscape rotation through `lv_display_set_rotation()`.
- Connects LVGL flush callback to `display_driver_draw_bitmap()`.
- Uses an LCD transfer-complete callback to know when SPI flush is done.
- Uses a FreeRTOS semaphore as `flush_wait_cb` so LVGL waits without busy-looping.
- Starts an ESP timer that increments LVGL tick every `LVGL_TICK_PERIOD_MS`.
- Calls `lv_timer_handler()` from a 24 KB UI task every 33 ms.
- Provides a mutex for controlled LVGL access from the current application
  tasks.
- Provides optional demo label creation and a 4 KB demo task.

## Public API

| API | Role |
| --- | --- |
| `ui_manager_lvgl_init()` | Initialize LVGL, display buffers, flush synchronization, and the 1 ms tick timer. |
| `ui_manager_lvgl_task_handler()` | Run one mutex-protected `lv_timer_handler()` iteration. |
| `ui_manager_lvgl_start_UI_task()` | Start the continuously running LVGL handler task. |
| `ui_manager_lvgl_wait_for_mutex()` | Lock LVGL before direct API access from another task. |
| `ui_manager_lvgl_release_mutex()` | Unlock LVGL after direct API access. |
| `ui_manager_lvgl_create_demo_screen()` | Create the static `LVGL OK` demo screen. |
| `ui_manager_lvgl_running_demo()` | Demo task entry point that updates and moves a counter label. |
| `ui_manager_lvgl_start_running_demo_task()` | Start the optional demo task. |
| `lvgl_task_handler()` | Public task entry point used by `ui_manager_lvgl_start_UI_task()`. |

## How To Use

Initialize the display driver first:

```c
display_driver_handle_t display_handle = {0};
ESP_ERROR_CHECK(display_driver_init(&display_handle));
```

Initialize LVGL manager:

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
```

Create the initial screen:

```c
ui_manager_lvgl_create_demo_screen();
```

Start the LVGL handler task:

```c
ui_manager_lvgl_start_UI_task();
```

If updating LVGL objects manually from another task, protect the call:

```c
ui_manager_lvgl_wait_for_mutex();
lv_label_set_text(label, "new text");
ui_manager_lvgl_release_mutex();
```

## Important Notes

- `esp_timer_start_periodic()` uses microseconds. The code multiplies
  `LVGL_TICK_PERIOD_MS` by 1000 so a value of `1` means 1 ms.
- The flush callback starts an asynchronous SPI transfer. LVGL must wait until
  the LCD IO callback signals completion.
- The flush wait semaphore avoids the LVGL v9 busy-wait path that can trigger
  the task watchdog.
- RGB565 byte swapping is currently enabled with `LCD_SWAP_RGB565_BYTES`.
- Landscape rotation costs extra RAM because a rotate buffer is allocated.
- The configured rotation is currently `LV_DISPLAY_ROTATION_270`, so LVGL's
  logical screen is landscape after initialization.
- `LCD_LVGL_DRAW_BUF_LINES` controls partial draw buffer height, not full screen
  resolution.
- `ui_manager_lvgl_wait_for_mutex()` waits indefinitely. Only call it after
  successful `ui_manager_lvgl_init()`, keep the locked section short, and
  always pair it with `ui_manager_lvgl_release_mutex()`.
- There is currently no UI callback queue. Application tasks that call LVGL
  directly must use the manager mutex.

## Future Attention

- Check `s_lvgl_display != NULL` before calling `lv_display_set_buffers()`.
- Add init/deinit guards if the manager must support repeated initialization.
- Remove or separate demo helpers once the real dashboard UI exists.
- Add labels/widgets for Wi-Fi, sensor, SD, and cloud state in later sprints.
