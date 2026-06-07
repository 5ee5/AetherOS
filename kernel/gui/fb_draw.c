#include "gui/fb_draw.h"
#include "gui/font8x16.h"
#include "os/bootinfo.h"

#include <stdint.h>

static volatile uint32_t *s_fb;
static uint32_t s_width;
static uint32_t s_height;
static uint32_t s_stride;

/* Called from kernel_main after fb_init. */
void fb_draw_init(void)
{
    /* Read state set by fb_init via the same statics.
       We re-read from a global symbol exposed by fb.c. */
    extern volatile uint32_t *fb_get_base(void);
    extern uint32_t fb_get_width(void);
    extern uint32_t fb_get_height(void);
    extern uint32_t fb_get_stride(void);
    s_fb     = fb_get_base();
    s_width  = fb_get_width();
    s_height = fb_get_height();
    s_stride = fb_get_stride();
}

uint32_t fb_draw_width(void)  { return s_width; }
uint32_t fb_draw_height(void) { return s_height; }

void fb_draw_pixel(int x, int y, uint32_t color)
{
    if ((uint32_t)x >= s_width || (uint32_t)y >= s_height) return;
    s_fb[(uint32_t)y * s_stride + (uint32_t)x] = color;
}

void fb_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; ++row)
        for (int col = x; col < x + w; ++col)
            fb_draw_pixel(col, row, color);
}

void fb_draw_hline(int x, int y, int w, uint32_t color)
{
    for (int i = 0; i < w; ++i) fb_draw_pixel(x + i, y, color);
}

void fb_draw_vline(int x, int y, int h, uint32_t color)
{
    for (int i = 0; i < h; ++i) fb_draw_pixel(x, y + i, color);
}

void fb_draw_border(int x, int y, int w, int h, uint32_t color)
{
    fb_draw_hline(x, y,         w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x,         y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

void fb_draw_char(int px, int py, char c, uint32_t fg, uint32_t bg)
{
    unsigned char uc = (unsigned char)c;
    if (uc < 32 || uc > 127) uc = '?';
    const uint8_t *glyph = font8x16[uc - 32];
    for (int row = 0; row < FONT_H; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; ++col) {
            uint32_t color = (bits & (0x80U >> col)) ? fg : bg;
            fb_draw_pixel(px + col, py + row, color);
        }
    }
}

int fb_draw_string(int px, int py, const char *s, uint32_t fg, uint32_t bg)
{
    int x = px;
    while (*s) {
        if (*s == '\n') { x = px; py += FONT_H; }
        else { fb_draw_char(x, py, *s, fg, bg); x += FONT_W; }
        ++s;
    }
    return x;
}

int fb_draw_dec(int px, int py, uint64_t n, uint32_t fg, uint32_t bg)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (n == 0) buf[--i] = '0';
    while (n > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    return fb_draw_string(px, py, buf + i, fg, bg);
}
