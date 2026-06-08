#ifndef KERNEL_PROC_PIPE_H
#define KERNEL_PROC_PIPE_H

#include <stdbool.h>
#include <stdint.h>

int  pipe_alloc(void);
void pipe_free(int idx);
int  pipe_write(int idx, const void *buf, uint32_t len);
int  pipe_read(int idx, void *buf, uint32_t len);
void pipe_close_write(int idx);
void pipe_close_read(int idx);

#endif
