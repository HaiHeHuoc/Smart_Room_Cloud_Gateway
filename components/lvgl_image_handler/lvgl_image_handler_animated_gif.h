/*
 * SPDX-License-Identifier: Apache-2.0
 * Derived from AnimatedGIF, Copyright 2020 BitBank Software, Inc.
 *
 * Local configuration wrapper for LVGL's Apache-2.0 AnimatedGIF decoder.
 *
 * The upstream LVGL copy fixes MAX_WIDTH at 480 pixels. This component uses
 * the same decoder with a wider one-line buffer so large source GIFs can be
 * downscaled without allocating a source-sized framebuffer.
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"

/* Prevent the wrapped gif.c file from loading its fixed-width header. */
#define __ANIMATEDGIF__

#define MAX_CHUNK_SIZE 255
#define TURBO_BUFFER_SIZE 0x6100
#define MAX_CODE_SIZE 12
#define MAX_COLORS 256
#define MAX_WIDTH 4096
#define LZW_BUF_SIZE (6 * MAX_CHUNK_SIZE)
#define LZW_HIGHWATER (4 * MAX_CHUNK_SIZE)
#define FILE_BUF_SIZE (1 << MAX_CODE_SIZE)
#define PIXEL_FIRST 0
#define PIXEL_LAST (1 << MAX_CODE_SIZE)
#define LINK_UNUSED 5911
#define LINK_END 5912
#define MAX_HASH 5003
#define LZW_BUF_SIZE_TURBO \
    (LZW_BUF_SIZE + (2 << MAX_CODE_SIZE) + (PIXEL_LAST * 2) + MAX_WIDTH)
#define LZW_HIGHWATER_TURBO ((LZW_BUF_SIZE_TURBO * 14) / 16)

enum {
    GIF_PALETTE_RGB565_LE = 0,
    GIF_PALETTE_RGB565_BE,
    GIF_PALETTE_RGB888,
    GIF_PALETTE_RGB8888,
    GIF_PALETTE_1BPP,
    GIF_PALETTE_1BPP_OLED
};

enum {
    GIF_DRAW_RAW = 0,
    GIF_DRAW_COOKED
};

enum {
    GIF_SUCCESS = 0,
    GIF_DECODE_ERROR,
    GIF_TOO_WIDE,
    GIF_INVALID_PARAMETER,
    GIF_UNSUPPORTED_FEATURE,
    GIF_FILE_NOT_OPEN,
    GIF_EARLY_EOF,
    GIF_EMPTY_FRAME,
    GIF_BAD_FILE,
    GIF_ERROR_MEMORY
};

typedef struct gif_file_tag {
    int32_t iPos;
    int32_t iSize;
    uint8_t *pData;
    lv_fs_file_t fHandle;
} GIFFILE;

typedef struct gif_info_tag {
    int32_t iFrameCount;
    int32_t iDuration;
    int32_t iMaxDelay;
    int32_t iMinDelay;
} GIFINFO;

typedef struct gif_draw_tag {
    int iX;
    int iY;
    int y;
    int iWidth;
    int iHeight;
    int iCanvasWidth;
    void *pUser;
    uint8_t *pPixels;
    uint16_t *pPalette;
    uint8_t *pPalette24;
    uint8_t ucTransparent;
    uint8_t ucHasTransparency;
    uint8_t ucDisposalMethod;
    uint8_t ucBackground;
    uint8_t ucPaletteType;
    uint8_t ucIsGlobalPalette;
} GIFDRAW;

typedef int32_t (GIF_READ_CALLBACK)(GIFFILE *file,
                                    uint8_t *buffer,
                                    int32_t length);
typedef int32_t (GIF_SEEK_CALLBACK)(GIFFILE *file, int32_t position);
typedef void (GIF_DRAW_CALLBACK)(GIFDRAW *draw);
typedef void *(GIF_OPEN_CALLBACK)(const char *filename, int32_t *file_size);
typedef void (GIF_CLOSE_CALLBACK)(lv_fs_file_t *handle);
typedef void *(GIF_ALLOC_CALLBACK)(uint32_t size);
typedef void (GIF_FREE_CALLBACK)(void *buffer);

typedef struct gif_image_tag {
    uint16_t iWidth;
    uint16_t iHeight;
    uint16_t iCanvasWidth;
    uint16_t iCanvasHeight;
    uint16_t iX;
    uint16_t iY;
    uint16_t iBpp;
    int16_t iError;
    uint16_t iFrameDelay;
    int16_t iRepeatCount;
    uint16_t iXCount;
    uint16_t iYCount;
    int iLZWOff;
    int iLZWSize;
    int iCommentPos;
    short sCommentLen;
    unsigned char bEndOfFrame;
    unsigned char ucGIFBits;
    unsigned char ucBackground;
    unsigned char ucTransparent;
    unsigned char ucCodeStart;
    unsigned char ucMap;
    unsigned char bUseLocalPalette;
    unsigned char ucPaletteType;
    unsigned char ucDrawType;
    GIF_READ_CALLBACK *pfnRead;
    GIF_SEEK_CALLBACK *pfnSeek;
    GIF_DRAW_CALLBACK *pfnDraw;
    GIF_OPEN_CALLBACK *pfnOpen;
    GIF_CLOSE_CALLBACK *pfnClose;
    GIFFILE GIFFile;
    void *pUser;
    unsigned char *pFrameBuffer;
    unsigned char *pTurboBuffer;
    unsigned char *pPixels;
    unsigned char *pOldPixels;
    unsigned char ucFileBuf[FILE_BUF_SIZE];
    unsigned short pPalette[(MAX_COLORS * 3) / 2];
    unsigned short pLocalPalette[(MAX_COLORS * 3) / 2];
    unsigned char ucLZW[LZW_BUF_SIZE];
    unsigned short usGIFTable[1 << MAX_CODE_SIZE];
    unsigned char ucGIFPixels[PIXEL_LAST * 2];
    unsigned char ucLineBuf[MAX_WIDTH];
} GIFIMAGE;

/* Keep the wrapped decoder symbols private to this component. */
#define GIF_openRAM lvgl_image_handler_animated_gif_open_ram
#define GIF_openFile lvgl_image_handler_animated_gif_open_file
#define GIF_close lvgl_image_handler_animated_gif_close
#define GIF_begin lvgl_image_handler_animated_gif_begin
#define GIF_reset lvgl_image_handler_animated_gif_reset
#define GIF_playFrame lvgl_image_handler_animated_gif_play_frame
#define GIF_getCanvasWidth lvgl_image_handler_animated_gif_get_canvas_width
#define GIF_getCanvasHeight lvgl_image_handler_animated_gif_get_canvas_height
#define GIF_getComment lvgl_image_handler_animated_gif_get_comment
#define GIF_getInfo lvgl_image_handler_animated_gif_get_info
#define GIF_getLastError lvgl_image_handler_animated_gif_get_last_error
#define GIF_getLoopCount lvgl_image_handler_animated_gif_get_loop_count

int GIF_openRAM(GIFIMAGE *gif,
                uint8_t *data,
                int data_size,
                GIF_DRAW_CALLBACK *draw_cb);
int GIF_openFile(GIFIMAGE *gif,
                 const char *filename,
                 GIF_DRAW_CALLBACK *draw_cb);
void GIF_close(GIFIMAGE *gif);
void GIF_begin(GIFIMAGE *gif, unsigned char palette_type);
void GIF_reset(GIFIMAGE *gif);
int GIF_playFrame(GIFIMAGE *gif,
                  int *delay_ms,
                  void *user_data);
int GIF_getCanvasWidth(GIFIMAGE *gif);
int GIF_getCanvasHeight(GIFIMAGE *gif);
int GIF_getComment(GIFIMAGE *gif, char *destination);
int GIF_getInfo(GIFIMAGE *gif, GIFINFO *info);
int GIF_getLastError(GIFIMAGE *gif);
int GIF_getLoopCount(GIFIMAGE *gif);

#define REGISTER_WIDTH 32

#ifdef ALLOWS_UNALIGNED
#define INTELSHORT(pointer) (*(uint16_t *)(pointer))
#define INTELLONG(pointer) (*(uint32_t *)(pointer))
#else
#define INTELSHORT(pointer) \
    ((*pointer) + (*((pointer) + 1) << 8))
#define INTELLONG(pointer)                                      \
    ((*pointer) + (*((pointer) + 1) << 8) +                    \
     (*((pointer) + 2) << 16) + (*((pointer) + 3) << 24))
#endif

#define BIGINT int32_t
#define BIGUINT uint32_t
