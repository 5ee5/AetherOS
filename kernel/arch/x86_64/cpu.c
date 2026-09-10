#include "arch/x86_64/cpu.h"

#include <stdint.h>

#include "core/panic.h"
#include "core/serial.h"

#define IA32_KERNEL_GS_BASE 0xc0000102U
#define IA32_EFER           0xc0000080U
#define EFER_NXE            (1ULL << 11U)
#define CR4_SMEP            (1ULL << 20U)
#define CR4_OSFXSR     (1UL << 9U)
#define CR4_OSXMMEXCPT (1UL << 10U)
#define CR0_MP         (1UL << 1U)
#define CR0_EM         (1UL << 2U)
#define CR0_TS         (1UL << 3U)
#define CR0_NE         (1UL << 5U)
#define CR4_SMAP            (1ULL << 21U)

struct cpu_local cpu_local_data[MAX_CPUS];

/* stac/clac raise #UD unless CR4.SMAP is set, so every user-memory access has
   to know whether the feature is actually on.  All CPUs enable the same set. */
bool cpu_smap_active;

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

static void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                        uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

/* Enable the hardware barriers between kernel and user memory.  Must run on
   every CPU: EFER and CR4 are per-CPU registers, so an AP that skipped this
   would execute the same kernel with the protections silently off.

   NXE only makes PTE bit 63 meaningful; nothing becomes non-executable until a
   mapping actually sets it.  SMEP stops the kernel executing user pages.  SMAP
   stops it reading or writing them outside an explicit stac/clac window, which
   is why it must not be turned on before copy_to/from_user and the argv setup
   in spawn_process bracket their accesses. */
void cpu_enable_protections(void)
{
    uint32_t a, b, c, d;

    cpuid_count(0x80000001U, 0, &a, &b, &c, &d);
    if (d & (1U << 20U)) {
        wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_NXE);
    }

    cpuid_count(7U, 0, &a, &b, &c, &d);
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (b & (1U << 7U))  cr4 |= CR4_SMEP;
    if (b & (1U << 20U)) cr4 |= CR4_SMAP;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    if (b & (1U << 20U)) cpu_smap_active = true;

    /* Turn on FXSAVE/FXRSTOR and SSE exceptions explicitly, and initialise the
       FPU, on every CPU.  None of this used to be touched anywhere: SSE worked
       only because the UEFI firmware happened to leave CR4.OSFXSR set, which is
       not something to rely on -- and the scheduler now saves and restores FPU
       state through fxsave64/fxrstor64, which #UD without OSFXSR and #NM if
       CR0.TS is left set.  CR0.EM must be clear (no x87 emulation) and CR0.MP
       set so WAIT/FWAIT behaves. */
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |=  (CR0_MP | CR0_NE);
    cr0 &= ~(CR0_EM | CR0_TS);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile("fninit");

    /* Report what actually took effect: on a CPU without these features the
       calls above are silent no-ops, and "the tests still pass" would prove
       nothing about whether the protections are on. */
    serial_write("cpu: protections nx=");
    serial_write((rdmsr(IA32_EFER) & EFER_NXE) ? "on" : "off");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    serial_write(" smep=");
    serial_write((cr4 & CR4_SMEP) ? "on" : "off");
    serial_write(" smap=");
    serial_write((cr4 & CR4_SMAP) ? "on" : "off");
    serial_write(" fxsr=");
    serial_write((cr4 & CR4_OSFXSR) ? "on" : "off");
    serial_write("\n");
}

void cpu_local_init(uint32_t lapic_id)
{
    if (lapic_id >= MAX_CPUS) {
        panic("cpu_local_init: LAPIC ID exceeds MAX_CPUS");
    }
    uint64_t addr = (uint64_t)(uintptr_t)&cpu_local_data[lapic_id];
    wrmsr(IA32_KERNEL_GS_BASE, addr);
}
