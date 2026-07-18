/*
 * Compile LVGL's bundled AnimatedGIF decoder with the local scanline width
 * and symbol prefix declared in lvgl_image_handler_animated_gif.h.
 *
 * This translation unit intentionally includes the upstream C implementation
 * so its compile-time configuration applies without modifying managed LVGL.
 */
/* Includes ----------------------------------------------------------------- */
#include "lvgl_image_handler_animated_gif.h"

#include "src/libs/gif/AnimatedGIF/src/gif.c"
