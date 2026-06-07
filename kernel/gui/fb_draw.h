#ifndef KERNEL_GUI_FB_DRAW_H
#define KERNEL_GUI_FB_DRAW_H

#include <stdint.h>

/* Initialise drawing layer (call after fb_init). */
void fb_draw_init(void);

uint32_t fb_draw_width(void);
uint32_t fb_draw_height(void);

void fb_draw_pixel(int x, int y, uint32_t color);
void fb_draw_rect(int x, int y, int w, int h, uint32_t color);
void fb_draw_hline(int x, int y, int w, uint32_t color);
void fb_draw_vline(int x, int y, int h, uint32_t color);

/* Draw a border rectangle (outline only). */
void fb_draw_border(int x, int y, int w, int h, uint32_t color);

/* Draw a character (8x16 glyph) at pixel position (px, py). */
void fb_draw_char(int px, int py, char c, uint32_t fg, uint32_t bg);

/* Draw a null-terminated string. Returns end x position. */
int fb_draw_string(int px, int py, const char *s, uint32_t fg, uint32_t bg);

/* Draw a decimal number. */
int fb_draw_dec(int px, int py, uint64_t n, uint32_t fg, uint32_t bg);

#endif
