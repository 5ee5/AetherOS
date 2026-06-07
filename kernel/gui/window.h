#ifndef KERNEL_GUI_WINDOW_H
#define KERNEL_GUI_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#define WIN_TITLE_H    20
#define WIN_BORDER     2
#define WIN_MAX_TITLE  48
#define WIN_MAX        8

typedef struct window {
    bool     in_use;
    int      x, y, w, h;    /* outer bounds including title bar + border */
    char     title[WIN_MAX_TITLE];
    /* Colors */
    uint32_t title_active;
    uint32_t title_inactive;
    uint32_t client_bg;
    bool     focused;
    /* Simple text console inside the window */
    int      cursor_x, cursor_y;  /* character grid position */
    uint32_t fg, bg;
} window_t;

void     win_init(void);
window_t *win_create(const char *title, int x, int y, int w, int h);
void     win_destroy(window_t *w);
void     win_draw(window_t *w);
void     win_draw_all(void);
void     win_print(window_t *w, const char *s);
void     win_printf_dec(window_t *w, uint64_t n);
window_t *win_get(int index);   /* 0-based */

#endif
