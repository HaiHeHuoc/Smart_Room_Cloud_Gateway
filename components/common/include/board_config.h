#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* =========================================================================
 * Board Configuration
 * Target : ESP32-S3
 * LCD    : ST7735 Green Tab, 128x160 SPI TFT
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

#define LCD_PIXEL_CLOCK_HZ (4 * 1000 * 1000) // 4 MHz

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
