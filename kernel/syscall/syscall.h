#ifndef KERNEL_SYSCALL_SYSCALL_H
#define KERNEL_SYSCALL_SYSCALL_H

#include <stdint.h>

/* Set up EFER.SCE, STAR, LSTAR, FMASK MSRs. */
void syscall_init(void);

/* Called from syscall_entry.asm with (nr, a0..a4) in rdi,rsi,rdx,r10,r8,r9.
   Returns value placed in rax on sysretq. */
int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4);

#endif
