#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* =========================================================================
 * ST7735 LCD Device Configuration
 *
 * Includes:
 * - LCD controller and panel variant
 * - SPI host and display resolution
 * - LCD GPIO pin mapping
 * - SPI command/parameter bit width
 * - Backlight control level
 * - SPI transfer size
 * - LVGL draw buffer settings for this LCD
 * ========================================================================= */
/* LCD Controller / Variant ------------------------------------------------ */
#define LCD_CONTROLLER_ST7735            1
#define LCD_ST7735_VARIANT_GREENTAB      1

/* LCD Basic Configuration ------------------------------------------------- */

/*
 * ST7735 LCD 128x160 usually uses SPI.
 * For ESP32-S3, SPI2_HOST is a common choice for external SPI devices.
 */

#define LCD_SPI_HOST SPI2_HOST

#define LCD_H_RES 128
#define LCD_V_RES 160

#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000) // 10 MHz

/* LCD GPIO Pin Mapping ---------------------------------------------------- */
/*
 *
 * LCD SCL / SCK  -> LCD_GPIO_SCLK
 * LCD SDA / MOSI -> LCD_GPIO_MOSI
 * LCD CS         -> LCD_GPIO_CS
 * LCD DC / A0    -> LCD_GPIO_DC
 * LCD RST / RES  -> LCD_GPIO_RST
 * LCD BL / LED   -> LCD_GPIO_BL
 */

#define LCD_GPIO_MISO               GPIO_NUM_NC
#define LCD_GPIO_MOSI               GPIO_NUM_11
#define LCD_GPIO_SCLK               GPIO_NUM_12
#define LCD_GPIO_CS                 GPIO_NUM_10
#define LCD_GPIO_DC                 GPIO_NUM_13
#define LCD_GPIO_RST                GPIO_NUM_14
#define LCD_GPIO_BL                 GPIO_NUM_15

/* LCD Signal Configuration ------------------------------------------------ */
#define LCD_CMD_BITS                8
#define LCD_PARAM_BITS              8

#define LCD_BACKLIGHT_ON_LEVEL      1
#define LCD_BACKLIGHT_OFF_LEVEL     0

/* LCD Transfer Configuration --------------------------------------------- */
#define LCD_MAX_TRANSFER_SIZE            (LCD_H_RES * LCD_V_RES * sizeof(uint16_t))

/* LVGL Configuration ------------------------------------------------------ */

/*
 * Partial draw buffer.
 * 128 * 20 pixels * 2 bytes = 5120 bytes.
 * This is safer than using full screen buffer at the beginning.
 */
#define LCD_LVGL_DRAW_BUF_LINES     20




/* =========================================================================
 * Device: ST7735 SPI TFT SD slot
 * ========================================================================= */
#define SD_SPI_HOST      SPI3_HOST

#define SD_GPIO_MOSI     GPIO_NUM_16
#define SD_GPIO_MISO     GPIO_NUM_17
#define SD_GPIO_SCLK     GPIO_NUM_18
#define SD_GPIO_CS       GPIO_NUM_8

#define SD_MOUNT_POINT           "/sdcard"

/* ESP-IDF SDSPI uses kHz; 4 MHz still requires cold-start hardware tests. */
#define SD_CLOCK_KHZ (4 * 1000)

/*
* Maximum number of files that can be opened at the same time.
* This does NOT limit the total number of files stored on the SD card.
*/
#define SD_MAX_FILES             5

/*
* FATFS allocation unit / cluster size used when formatting the card.
* 16 * 1024 = 16 KB.
* This does NOT limit SD card capacity.
*/
#define SD_ALLOCATION_UNIT_SIZE  (16 * 1024)

#define SD_CARD_MANAGER_PATH_MAX_LEN    256

/* =========================================================================
 * Sensor Manager
 * ========================================================================= */
// DHT22
#define DHT22_PIN_GPIO                  GPIO_NUM_4

/* =========================================================================
 * Button Manager
 * ========================================================================= */

#define FACTORY_RESET_BUTTON_GPIO               GPIO_NUM_9

#define FACTORY_RESET_BUTTON_ACTIVE_LEVEL       1

#define FACTORY_RESET_BUTTON_POLL_PERIOD_MS     10U
#define FACTORY_RESET_BUTTON_DEBOUNCE_MS        40U
#define FACTORY_RESET_BUTTON_LONG_PRESS_MS      5000U

/* Audio */
#define AUDIO_GPIO_BCLK        GPIO_NUM_47
#define AUDIO_GPIO_WS          GPIO_NUM_21
#define AUDIO_GPIO_MIC_DIN     GPIO_NUM_2
#define AUDIO_GPIO_SPK_DOUT    GPIO_NUM_7

#define AUDIO_SAMPLE_RATE_HZ   16000
