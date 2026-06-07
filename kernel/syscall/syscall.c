#include "syscall/syscall.h"

#include <stdint.h>

#include "core/serial.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "mem/vmm.h"

#define IA32_EFER  0xc0000080U
#define IA32_STAR  0xc0000081U
#define IA32_LSTAR 0xc0000082U
#define IA32_FMASK 0xc0000084U

/* syscall entry point declared in syscall_entry.asm */
extern void syscall_entry(void);

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xffffffffULL);
    uint32_t hi = (uint32_t)(value >> 32U);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32U) | lo;
}

void syscall_init(void)
{
    /* Enable SYSCALL/SYSRET: set SCE bit in IA32_EFER. */
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1U);

    /* STAR[47:32] = kernel CS (0x08); SYSRET uses base 0x20:
       SYSRET SS = (0x20+8)|3 = 0x2b, CS64 = (0x20+16)|3 = 0x33. */
    wrmsr(IA32_STAR, (0x0020ULL << 48U) | (0x0008ULL << 32U));

    /* LSTAR = syscall entry RIP. */
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* FMASK: mask IF on SYSCALL entry (cleared; re-enabled on SYSRETQ). */
    wrmsr(IA32_FMASK, 0x200ULL);

    serial_write("syscall: SYSCALL/SYSRET enabled\n");
}

/* ---- Syscall handlers -------------------------------------------------- */

#define SYS_WRITE  1U
#define SYS_EXIT   60U

static int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count)
{
    /* Only fd=1 (stdout) supported; write to serial. */
    if (fd != 1U) {
        return -9;   /* -EBADF */
    }
    /* Basic bounds check: user pointers must be below the kernel half. */
    if (buf_virt + count < buf_virt || buf_virt + count > 0x800000000000ULL) {
        return -14;  /* -EFAULT */
    }
    const char *p = (const char *)(uintptr_t)buf_virt;
    for (uint64_t i = 0; i < count; ++i) {
        serial_write_char(p[i]);
    }
    return (int64_t)count;
}

static int64_t sys_exit(uint64_t status)
{
    (void)status;

    struct thread *t = sched_current();
    if (t->is_user && t->cr3 != 0) {
        vmm_space_destroy(t->cr3);
        t->cr3 = 0;
    }

    /* Disable interrupts, mark dead, and force a reschedule. */
    __asm__ volatile("cli" ::: "memory");
    t->state = THREAD_DEAD;
    __asm__ volatile("int $0x20" ::: "memory");
    /* Never reached. */
    for (;;) {
        __asm__ volatile("hlt");
    }
    return 0;
}

int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4)
{
    (void)a3; (void)a4;
    switch (nr) {
    case SYS_WRITE:  return sys_write(a0, a1, a2);
    case SYS_EXIT:   return sys_exit(a0);
    default:
        serial_write("syscall: unknown nr=");
        serial_write_dec(nr);
        serial_write("\n");
        return -38;   /* -ENOSYS */
    }
}
