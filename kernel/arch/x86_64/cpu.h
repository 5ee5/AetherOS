#ifndef KERNEL_ARCH_X86_64_CPU_H
#define KERNEL_ARCH_X86_64_CPU_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Per-CPU data block. The address of cpu_local_data[lapic_id] is stored in
 * IA32_KERNEL_GS_BASE. The syscall entry stub uses swapgs to access it.
 *
 * Layout is ABI: syscall_entry.asm accesses offsets 0 and 8 directly.
 */
struct cpu_local {
    uint64_t kernel_rsp;   /* offset 0: kernel stack top for current user thread */
    uint64_t user_rsp;     /* offset 8: user RSP saved on syscall entry */
};

#define MAX_CPUS 64

extern struct cpu_local cpu_local_data[MAX_CPUS];

/* Called once per CPU (BSP in sched_init, APs in sched_ap_enter).
   Writes IA32_KERNEL_GS_BASE to point at cpu_local_data[lapic_id]. */
void cpu_local_init(uint32_t lapic_id);

/* True once CR4.SMAP is enabled; gates the stac/clac in the user-copy paths,
   which are #UD instructions when SMAP is off. */
extern bool cpu_smap_active;

/* Enable NX, SMEP and SMAP where the CPU supports them. Must be called on
   every CPU (EFER and CR4 are per-CPU), before any user process runs. */
void cpu_enable_protections(void);

#endif
