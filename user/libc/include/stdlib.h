#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void  *malloc(size_t n);
void   free(void *p);
int    atoi(const char *s);
void   exit(int status);

#endif
