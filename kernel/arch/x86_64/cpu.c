#include "arch/x86_64/cpu.h"

#include <stdint.h>

#include "core/panic.h"

#define IA32_KERNEL_GS_BASE 0xc0000102U

struct cpu_local cpu_local_data[MAX_CPUS];

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xffffffffULL);
    uint32_t hi = (uint32_t)(value >> 32U);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void cpu_local_init(uint32_t lapic_id)
{
    if (lapic_id >= MAX_CPUS) {
        panic("cpu_local_init: LAPIC ID exceeds MAX_CPUS");
    }
    uint64_t addr = (uint64_t)(uintptr_t)&cpu_local_data[lapic_id];
    wrmsr(IA32_KERNEL_GS_BASE, addr);
}
