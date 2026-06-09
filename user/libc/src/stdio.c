#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

/* Output buffer flushed per printf call. */
#define OBUF_SZ 512
static char   s_obuf[OBUF_SZ];
static size_t s_opos;

static void flush(void)
{
    if (s_opos) {
        write(1, s_obuf, s_opos);
        s_opos = 0;
    }
}

static void emit(char c)
{
    s_obuf[s_opos++] = c;
    if (s_opos == OBUF_SZ) flush();
}

static void emit_str(const char *s)
{
    if (!s) s = "(null)";
    while (*s) emit(*s++);
}

static void emit_uint(unsigned long v, unsigned base, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[20];
    int  i = 0;
    if (v == 0) { emit('0'); return; }
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    while (i--) emit(buf[i]);
}

static void emit_long(long v)
{
    if (v < 0) { emit('-'); emit_uint((unsigned long)-v, 10, 0); }
    else emit_uint((unsigned long)v, 10, 0);
}

/* Generic formatter: writes to either stdout buffer or a bounded string buffer. */
typedef struct {
    char  *buf;    /* NULL → emit to stdout */
    size_t cap;
    size_t pos;
} fmt_ctx_t;

static void ctx_putc(fmt_ctx_t *ctx, char c)
{
    if (ctx->buf) {
        if (ctx->pos + 1 < ctx->cap) ctx->buf[ctx->pos++] = c;
    } else {
        emit(c);
    }
}

static void ctx_puts(fmt_ctx_t *ctx, const char *s)
{
    if (!s) s = "(null)";
    while (*s) ctx_putc(ctx, *s++);
}

static void ctx_uint_w(fmt_ctx_t *ctx, unsigned long v, unsigned base, int upper, int width)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[20];
    int  i = 0;
    if (v == 0) { buf[i++] = '0'; }
    else { while (v) { buf[i++] = digits[v % base]; v /= base; } }
    /* right-align: pad with spaces */
    for (int pad = width - i; pad > 0; pad--) ctx_putc(ctx, ' ');
    while (i--) ctx_putc(ctx, buf[i]);
}

static void ctx_uint(fmt_ctx_t *ctx, unsigned long v, unsigned base, int upper)
{
    ctx_uint_w(ctx, v, base, upper, 0);
}

static void ctx_long_w(fmt_ctx_t *ctx, long v, int width)
{
    if (v < 0) { ctx_putc(ctx, '-'); ctx_uint_w(ctx, (unsigned long)-v, 10, 0, width - 1); }
    else ctx_uint_w(ctx, (unsigned long)v, 10, 0, width);
}

static void ctx_long(fmt_ctx_t *ctx, long v)
{
    ctx_long_w(ctx, v, 0);
}

static int vfmt(fmt_ctx_t *ctx, const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { ctx_putc(ctx, *p); continue; }
        p++;
        /* parse optional width */
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        switch (*p) {
        case 's': ctx_puts(ctx, va_arg(ap, const char *));                        break;
        case 'd': ctx_long_w(ctx, (long)va_arg(ap, int), width);                  break;
        case 'u': ctx_uint_w(ctx, (unsigned long)va_arg(ap, unsigned), 10, 0, width); break;
        case 'x': ctx_uint_w(ctx, (unsigned long)va_arg(ap, unsigned), 16, 0, width); break;
        case 'X': ctx_uint_w(ctx, (unsigned long)va_arg(ap, unsigned), 16, 1, width); break;
        case 'l':
            p++;
            if (*p == 'l') p++; /* skip second 'l' in %llu — treat as %lu */
            if (*p == 'd') ctx_long_w(ctx, va_arg(ap, long), width);
            else if (*p == 'u') ctx_uint_w(ctx, (unsigned long)va_arg(ap, unsigned long), 10, 0, width);
            else if (*p == 'x') ctx_uint_w(ctx, va_arg(ap, unsigned long), 16, 0, width);
            else { ctx_putc(ctx, '%'); ctx_putc(ctx, 'l'); ctx_putc(ctx, *p); }
            break;
        case 'c': ctx_putc(ctx, (char)va_arg(ap, int));                            break;
        case '%': ctx_putc(ctx, '%');                                               break;
        default:  ctx_putc(ctx, '%'); ctx_putc(ctx, *p);                           break;
        }
    }
    if (ctx->buf && ctx->pos < ctx->cap) ctx->buf[ctx->pos] = '\0';
    return (int)ctx->pos;
}

int vprintf(const char *fmt, va_list ap)
{
    s_opos = 0;
    fmt_ctx_t ctx = { NULL, 0, 0 };
    int r = vfmt(&ctx, fmt, ap);
    flush();
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    fmt_ctx_t ctx = { buf, (size_t)-1, 0 };
    return vfmt(&ctx, fmt, ap);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    fmt_ctx_t ctx = { buf, n, 0 };
    return vfmt(&ctx, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s)
{
    size_t n = strlen(s);
    write(1, s, n);
    write(1, "\n", 1);
    return (int)n + 1;
}

int putchar(int c)
{
    char b = (char)c;
    write(1, &b, 1);
    return c;
}

int getchar(void)
{
    char c;
    ssize_t n = read(0, &c, 1);
    return (n <= 0) ? -1 : (unsigned char)c;
}
