#include "gui/desktop.h"
#include "gui/fb_draw.h"
#include "gui/window.h"
#include "gui/font8x16.h"

#include <stdint.h>

#include "core/serial.h"
#include "drivers/ps2kbd.h"
#include "lib/string.h"
#include "net/ipv4.h"

/* ---- Color palette ------------------------------------------------------- */
#define COL_DESKTOP_BG  0xFF1A1B2EU  /* dark purple-navy */
#define COL_TASKBAR_BG  0xFF181825U
#define COL_TASKBAR_FG  0xFFCDD6F4U
#define COL_CLOCK_FG    0xFF89DCEBU
#define COL_ACCENT      0xFF89B4FAU

/* ---- Layout -------------------------------------------------------------- */
#define TASKBAR_H   28
#define TASKBAR_Y(h) ((h) - TASKBAR_H)

static uint32_t s_scr_w;
static uint32_t s_scr_h;
static window_t *s_net;    /* network status window */

static uint64_t s_tick;    /* monotonic tick count (incremented per frame) */

/* ---- Desktop background with gradient ------------------------------------ */

static void draw_background(void)
{
    uint32_t h = s_scr_h - TASKBAR_H;
    for (uint32_t y = 0; y < h; ++y) {
        /* Interpolate from COL_DESKTOP_BG at top to slightly lighter at bottom */
        uint8_t v = (uint8_t)(y * 10 / h);
        uint32_t col = 0xFF000000U
            | (uint32_t)((0x1a + v) << 16)
            | (uint32_t)((0x1b + v) << 8)
            | (uint32_t)(0x2e + v);
        fb_draw_hline(0, (int)y, (int)s_scr_w, col);
    }
}

static void draw_taskbar(void)
{
    int ty = TASKBAR_Y(s_scr_h);
    fb_draw_rect(0, ty, (int)s_scr_w, TASKBAR_H, COL_TASKBAR_BG);
    fb_draw_hline(0, ty, (int)s_scr_w, COL_ACCENT);

    /* OS name */
    fb_draw_string(8, ty + (TASKBAR_H - FONT_H) / 2,
                   "ClaudeOS v0.5", COL_ACCENT, COL_TASKBAR_BG);
}

static void draw_clock(void)
{
    /* Fake time from tick counter (ticks ≈ ~50ms each → ~20Hz).
       Display as MM:SS since we have no RTC driver yet. */
    uint64_t secs  = s_tick / 20;
    uint64_t mins  = (secs / 60) % 60;
    uint64_t sec   = secs % 60;

    int ty = TASKBAR_Y(s_scr_h);
    int cx = (int)s_scr_w - 80;
    int cy = ty + (TASKBAR_H - FONT_H) / 2;

    /* Erase previous clock. */
    fb_draw_rect(cx, ty + 1, 72, TASKBAR_H - 2, COL_TASKBAR_BG);

    char buf[8];
    buf[0] = (char)('0' + mins / 10);
    buf[1] = (char)('0' + mins % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + sec / 10);
    buf[4] = (char)('0' + sec % 10);
    buf[5] = '\0';
    fb_draw_string(cx, cy, buf, COL_CLOCK_FG, COL_TASKBAR_BG);
}

static void draw_net_status(void)
{
    if (!s_net) return;
    /* Refresh the network window content every 60 ticks (~3s). */
    if (s_tick % 60 != 0) return;

    /* Re-draw the window frame. */
    win_draw(s_net);

    s_net->cursor_x = 0;
    s_net->cursor_y = 0;

    uint32_t ip = ipv4_our_ip();
    if (ip == 0) {
        win_print(s_net, "IP: not configured\n");
    } else {
        const uint8_t *b = (const uint8_t *)&ip;
        win_print(s_net, "IP: ");
        win_printf_dec(s_net, b[0]); win_print(s_net, ".");
        win_printf_dec(s_net, b[1]); win_print(s_net, ".");
        win_printf_dec(s_net, b[2]); win_print(s_net, ".");
        win_printf_dec(s_net, b[3]); win_print(s_net, "\n");

        uint32_t gw = ipv4_gateway();
        const uint8_t *g = (const uint8_t *)&gw;
        win_print(s_net, "GW: ");
        win_printf_dec(s_net, g[0]); win_print(s_net, ".");
        win_printf_dec(s_net, g[1]); win_print(s_net, ".");
        win_printf_dec(s_net, g[2]); win_print(s_net, ".");
        win_printf_dec(s_net, g[3]); win_print(s_net, "\n");
    }

    uint64_t t = s_tick / 20;
    win_print(s_net, "Up: ");
    win_printf_dec(s_net, t / 60);
    win_print(s_net, "m ");
    win_printf_dec(s_net, t % 60);
    win_print(s_net, "s\n");
}

/* ---- Terminal log (mirrors serial output) -------------------------------- */

static window_t *s_log;
static char      s_log_pending[512];
static uint32_t  s_log_len;

void desktop_log(const char *s)
{
    /* Called from serial write hook (not yet implemented) — for now just
       use it from the desktop tick to print status messages. */
    uint32_t len = (uint32_t)strlen(s);
    if (s_log_len + len >= sizeof(s_log_pending)) {
        s_log_len = 0;
    }
    memcpy(s_log_pending + s_log_len, s, len);
    s_log_len += len;
}

static void flush_log(void)
{
    if (!s_log || s_log_len == 0) return;
    s_log_pending[s_log_len] = '\0';
    win_print(s_log, s_log_pending);
    s_log_len = 0;
}

/* ---- Public API ---------------------------------------------------------- */

void desktop_init(void)
{
    s_scr_w = fb_draw_width();
    s_scr_h = fb_draw_height();

    draw_background();
    draw_taskbar();

    win_init();

    /* Terminal window */
    s_log = win_create("System Log", 10, 10, 500, 300);
    if (s_log) {
        s_log->fg = 0xFF89DCEBU;
        win_draw(s_log);
        win_print(s_log, "ClaudeOS kernel log\n");
        win_print(s_log, "====================\n");
        win_print(s_log, "All systems nominal.\n");
    }

    /* Network status window */
    s_net = win_create("Network", (int)s_scr_w - 220, 10, 210, 120);
    if (s_net) {
        win_draw(s_net);
        win_print(s_net, "Checking network...\n");
    }

    /* "About" window */
    window_t *about = win_create("About ClaudeOS",
                                  (int)s_scr_w / 2 - 180, (int)s_scr_h / 2 - 80,
                                  360, 160);
    if (about) {
        about->fg = 0xFFF38BA8U;
        win_draw(about);
        win_print(about, "  ClaudeOS v0.5 - Milestone 5\n");
        win_print(about, "  Built with pure C + NASM\n");
        win_print(about, "  UEFI boot, x86-64, SMP\n");
        win_print(about, "  Network, VFS, GUI\n");
    }
}

void desktop_tick(void)
{
    ++s_tick;

    /* Update clock every 20 ticks (~1s). */
    if (s_tick % 20 == 0) draw_clock();

    /* Network status. */
    draw_net_status();

    /* Flush any pending log text. */
    flush_log();

    /* Process keyboard input into the log window. */
    while (ps2kbd_ready()) {
        char c = ps2kbd_getchar();
        if (s_log && c) {
            char buf[2] = { c, '\0' };
            win_print(s_log, buf);
        }
    }
}
