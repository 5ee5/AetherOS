#ifndef KERNEL_LIB_STRING_H
#define KERNEL_LIB_STRING_H

#include <stddef.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
int memcmp(const void *lhs, const void *rhs, size_t count);
size_t strlen(const char *text);
int    strcmp(const char *a, const char *b);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strncat(char *dst, const char *src, size_t n);

#endif
