#ifndef KERNEL_GUI_FONT8X16_H
#define KERNEL_GUI_FONT8X16_H

#include <stdint.h>

/* 8x16 bitmap font for ASCII 32-127.
   Each entry is 16 bytes (one byte per row, bit 7 = leftmost pixel).
   Public domain — derived from the classic VGA BIOS font. */
extern const uint8_t font8x16[96][16];

#define FONT_W 8
#define FONT_H 16

#endif
