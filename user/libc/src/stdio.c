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

int vprintf(const char *fmt, va_list ap)
{
    s_opos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit(*p); continue; }
        p++;
        switch (*p) {
        case 's': emit_str(va_arg(ap, const char *));           break;
        case 'd': emit_long((long)va_arg(ap, int));             break;
        case 'u': emit_uint((unsigned long)va_arg(ap, unsigned), 10, 0); break;
        case 'x': emit_uint((unsigned long)va_arg(ap, unsigned), 16, 0); break;
        case 'X': emit_uint((unsigned long)va_arg(ap, unsigned), 16, 1); break;
        case 'l':
            p++;
            if (*p == 'd') emit_long(va_arg(ap, long));
            else if (*p == 'u') emit_uint((unsigned long)va_arg(ap, unsigned long), 10, 0);
            else if (*p == 'x') emit_uint(va_arg(ap, unsigned long), 16, 0);
            else { emit('%'); emit('l'); emit(*p); }
            break;
        case 'c': emit((char)va_arg(ap, int));                  break;
        case '%': emit('%');                                     break;
        default:  emit('%'); emit(*p);                          break;
        }
    }
    flush();
    return (int)s_opos;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
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
