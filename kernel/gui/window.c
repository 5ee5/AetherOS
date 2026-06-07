#include "gui/window.h"
#include "gui/fb_draw.h"
#include "gui/font8x16.h"

#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"

/* ---- Color palette ------------------------------------------------------- */
#define COL_TITLE_ACTIVE   0xFF3366CCU
#define COL_TITLE_INACTIVE 0xFF666688U
#define COL_TITLE_TEXT     0xFFFFFFFFU
#define COL_BORDER_LIGHT   0xFFCCCCCCU
#define COL_BORDER_DARK    0xFF444444U
#define COL_CLIENT_BG      0xFF1E1E2EU
#define COL_TEXT_FG        0xFFCDD6F4U

static window_t s_windows[WIN_MAX];

void win_init(void)
{
    memset(s_windows, 0, sizeof(s_windows));
}

window_t *win_create(const char *title, int x, int y, int w, int h)
{
    for (int i = 0; i < WIN_MAX; ++i) {
        if (!s_windows[i].in_use) {
            window_t *wn = &s_windows[i];
            wn->in_use         = true;
            wn->x = x; wn->y = y; wn->w = w; wn->h = h;
            wn->title_active   = COL_TITLE_ACTIVE;
            wn->title_inactive = COL_TITLE_INACTIVE;
            wn->client_bg      = COL_CLIENT_BG;
            wn->focused        = true;
            wn->fg             = COL_TEXT_FG;
            wn->bg             = COL_CLIENT_BG;
            wn->cursor_x       = 0;
            wn->cursor_y       = 0;

            int tlen = (int)strlen(title);
            if (tlen >= WIN_MAX_TITLE) tlen = WIN_MAX_TITLE - 1;
            memcpy(wn->title, title, (uint32_t)tlen);
            wn->title[tlen] = '\0';
            return wn;
        }
    }
    return NULL;
}

void win_destroy(window_t *w)
{
    if (w) w->in_use = false;
}

/* Client area rect (inside title bar and borders). */
static void client_rect(const window_t *w, int *cx, int *cy, int *cw, int *ch)
{
    *cx = w->x + WIN_BORDER;
    *cy = w->y + WIN_TITLE_H + WIN_BORDER;
    *cw = w->w - WIN_BORDER * 2;
    *ch = w->h - WIN_TITLE_H - WIN_BORDER * 2;
}

void win_draw(window_t *w)
{
    if (!w->in_use) return;

    /* Outer border */
    fb_draw_rect(w->x, w->y, w->w, w->h, COL_BORDER_DARK);

    /* Title bar */
    uint32_t tc = w->focused ? w->title_active : w->title_inactive;
    fb_draw_rect(w->x + WIN_BORDER, w->y + WIN_BORDER,
                 w->w - WIN_BORDER * 2, WIN_TITLE_H - WIN_BORDER,
                 tc);
    fb_draw_string(w->x + WIN_BORDER + 6,
                   w->y + WIN_BORDER + (WIN_TITLE_H - WIN_BORDER - FONT_H) / 2,
                   w->title, COL_TITLE_TEXT, tc);

    /* Client area background */
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    fb_draw_rect(cx, cy, cw, ch, w->client_bg);
}

void win_draw_all(void)
{
    for (int i = 0; i < WIN_MAX; ++i)
        if (s_windows[i].in_use) win_draw(&s_windows[i]);
}

/* Print text into the window's client console area. */
void win_print(window_t *w, const char *s)
{
    if (!w || !w->in_use) return;
    int cx, cy, cw, ch;
    client_rect(w, &cx, &cy, &cw, &ch);
    int cols = cw / FONT_W;
    int rows = ch / FONT_H;
    if (cols <= 0 || rows <= 0) return;

    while (*s) {
        if (*s == '\n') {
            /* Fill rest of line with bg. */
            int fill_x = cx + w->cursor_x * FONT_W;
            int fill_w = cw - w->cursor_x * FONT_W;
            if (fill_w > 0)
                fb_draw_rect(fill_x, cy + w->cursor_y * FONT_H, fill_w, FONT_H, w->bg);
            w->cursor_x = 0;
            w->cursor_y++;
        } else {
            fb_draw_char(cx + w->cursor_x * FONT_W,
                         cy + w->cursor_y * FONT_H,
                         *s, w->fg, w->bg);
            w->cursor_x++;
        }

        /* Wrap */
        if (w->cursor_x >= cols) { w->cursor_x = 0; w->cursor_y++; }

        /* Scroll: simple — just reset cursor for now (no scroll buffer). */
        if (w->cursor_y >= rows) {
            w->cursor_y = 0;
            fb_draw_rect(cx, cy, cw, ch, w->bg);
        }
        ++s;
    }
}

void win_printf_dec(window_t *w, uint64_t n)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (n == 0) buf[--i] = '0';
    while (n > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    win_print(w, buf + i);
}

window_t *win_get(int index)
{
    if (index < 0 || index >= WIN_MAX) return NULL;
    return s_windows[index].in_use ? &s_windows[index] : NULL;
}
