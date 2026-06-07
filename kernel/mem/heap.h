#ifndef KERNEL_MEM_HEAP_H
#define KERNEL_MEM_HEAP_H

#include <stdint.h>

void  heap_init(void);
void *kmalloc(uint64_t size);
void  kfree(void *ptr);

#endif
