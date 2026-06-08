#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

int  printf(const char *fmt, ...);
int  vprintf(const char *fmt, va_list ap);
int  sprintf(char *buf, const char *fmt, ...);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
int  vsprintf(char *buf, const char *fmt, va_list ap);
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  puts(const char *s);
int  putchar(int c);
int  getchar(void);

#endif
