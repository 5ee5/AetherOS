# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```sh
make              # build ESP tree (bootloader + kernel ELF)
make iso          # produce build/os.iso (requires xorriso + mtools)
make test         # boot smoke test via QEMU/OVMF (requires iso)
make run-qemu     # interactive QEMU session, serial on stdout
make clean        # wipe build/
./scripts/check-deps.sh   # verify required tools are present
```

There is no single-test target. The kernel test harness runs all suites at boot via `ktest_run_all()` in `kernel/kernel.c`. Test output goes to COM1 serial. The smoke test (`make test`) fails if `ktest: FAIL` appears or `ktest:` output is absent.

## Architecture

### Boot Flow

UEFI firmware → `build/BOOTX64.EFI` (PE32+ EFI app, built from `boot/uefi/main.c`) → loads `EFI/OS/KERNEL.ELF` from the same device, validates ELF64, maps PT_LOAD segments, builds 4-level paging, captures GOP/RSDP/memory map, calls `ExitBootServices`, switches to new CR3, jumps to `kernel_entry` in the higher half.

`include/os/bootinfo.h` defines `struct os_boot_info` — the only ABI contract between bootloader and kernel.

### Kernel Address Layout

| Region | Virtual | Physical |
|---|---|---|
| Kernel image | `0xffffffff80000000` | `0x00200000` |
| Higher-half direct map | `0xffff800000000000` | `0x0` |
| Early direct map size | 64 GiB | — |

The kernel is linked with `-mcmodel=kernel`. The linker script is `kernel/linker.ld`.

### Kernel Entry and Stack

`kernel/arch/x86_64/entry.asm` sets up the initial stack and calls `kernel_main`. The `.bss` section contains a 4096-byte `kernel_stack_guard` page immediately below the 64 KiB `kernel_stack`. This guard page is unmapped during `vmm_init` to catch stack overflows and null-pointer dereferences.

### Subsystem Initialization Order (`kernel/kernel.c`)

`serial_init` → validate boot info → `gdt_init` → `tss_init` → `syscall_init` → `idt_init` → `fb_init` → `pmm_init` → `vmm_init` → unmap stack guard → `heap_init` → `acpi_init` → `lapic_init` + `apic_timer_init` → `ioapic_init` → `sched_init` → `smp_init` → `ktest_run_all` → `process_create_from_elf` → idle loop.

`smp_init` pre-allocates AP idle threads (via `sched_ap_prepare`) before sending SIPI, so no allocations race with the test suite.

### Memory Management

**PMM** (`kernel/mem/pmm.c`): Bitmap allocator over the first 64 GiB of physical memory. One bit per 4 KiB page. Uses `__builtin_ctzll(~word)` for O(1) free-page scan per 64-page word. `PMM_ALLOC_FAILED = UINT64_MAX` is the sentinel (never a valid page address). Frees conventional memory and boot-services pages from the UEFI memory map.

**VMM** (`kernel/mem/vmm.c`): Walks the bootloader-installed 4-level page tables using the direct map for physical→virtual pointer conversion (`phys_to_virt`). Exposes `vmm_map` / `vmm_unmap` / `vmm_protect` for 4 KiB pages, and `vmm_virt_to_phys` for address translation. `ensure_table` allocates intermediate page table pages from PMM on demand. `vmm_pml4_phys()` returns the physical address of the active PML4 (used by SMP trampoline).

**Heap** (`kernel/mem/heap.c`): First-fit free-list allocator. Virtual base `0xffff900000000000`. `expand()` maps new PMM pages via VMM on demand. 16-byte aligned. Coalesces adjacent free blocks. `kmalloc(0)` returns NULL.

### ACPI

`kernel/acpi/acpi.c`: Parses RSDP → RSDT (ACPI 1.0) or XSDT (2.0+) → MADT. Collects LAPIC base, IOAPIC base/ID/GSI-base, and up to 64 CPU LAPIC IDs into `struct acpi_madt_info`. QEMU/OVMF uses ACPI 1.0 (revision 0, RSDT only).

### Local APIC and IOAPIC

`kernel/arch/x86_64/apic.c`: LAPIC mapped at virtual `0xffffa00000000000`. `lapic_init()` enables the LAPIC and spurious vector. `apic_timer_init()` calibrates the APIC timer against the PIT (~1 kHz, vector `0x20`). `lapic_ap_init()` initializes the LAPIC on each AP using the already-calibrated period. `lapic_send_ipi()` writes ICR and waits for delivery.

`kernel/arch/x86_64/ioapic.c`: IOAPIC mapped at virtual `0xffffa00000001000`. All 24 redirection entries masked at init. `ioapic_route()` / `ioapic_mask()` / `ioapic_unmask()` for later use.

### Scheduler and Threads

`kernel/sched/sched.c`: Round-robin preemptive scheduler. FIFO run queue protected by a spinlock. Per-CPU `cpu_current[]` and `cpu_idle[]` arrays indexed by LAPIC ID (up to 64 CPUs). `sched_tick()` is called from `apic_timer_isr` (assembly): saves old RSP, calls `lapic_eoi`, requeues running thread, picks next, returns new RSP. `sched_init()` installs the timer gate and creates BSP thread + idle thread.

`kernel/sched/thread.c`: 64 KiB kernel stack per thread. Initial stack frame is 20 qwords (15 GP registers + 5-word iretq frame). `thread_run_trampoline` (assembly) calls `fn(arg)` then `thread_exit`. `thread_exit` disables interrupts, marks thread DEAD, and triggers a timer interrupt to be rescheduled.

`kernel/arch/x86_64/sched_asm.asm`: `apic_timer_isr` saves/restores all 15 GP registers and calls `sched_tick`. `thread_run_trampoline` and `sched_idle_loop` (sti+hlt) also live here.

### SMP

`kernel/arch/x86_64/smp.c`: `smp_init()` copies the flat-binary trampoline to physical `0x8000`, pre-allocates AP idle threads on the BSP via `sched_ap_prepare()`, then sends INIT+SIPI to each non-BSP LAPIC ID. Waits for `TRAM_READY` flag set by trampoline. AP boot data at physical `0x8F00`–`0x8F40`: PML4 phys, entry point, stack top, ready flag, and a 32-byte GDT.

`kernel/arch/x86_64/smp_trampoline.asm`: Flat binary at org `0x8000`. 16-bit → 32-bit (protected, no paging) → 64-bit (copies CR3, enables paging) → jumps to `ap_entry()`. SIPI vector `0x08` → CS=`0x0800`, IP=0.

`ap_entry()`: runs `lapic_ap_init()`, then `sched_ap_enter()` (sets `cpu_current` to pre-allocated idle thread, no allocation), then falls into `sched_idle_loop()`.

### Synchronization

`kernel/sync/spinlock.c`: xchg-based test-and-set. Acquire spins with `pause`. Release is a store with compiler fence.

`kernel/sync/mutex.c`: Spinlock + intrusive wait queue. `mutex_lock()`: if locked, enqueue self, set `THREAD_BLOCKED`, release spinlock, trigger timer ISR to yield, loop. `mutex_unlock()`: clear locked, if waiters: pop one and call `sched_wake()`.

### IDT and Exceptions

`kernel/arch/x86_64/interrupts.asm` defines ISR stubs via `ISR_NOERR`/`ISR_ERR` macros. `kernel/arch/x86_64/idt.c` installs gates for vectors 0–31 and sets `idt[8].ist = 1` for double fault (uses IST1 from TSS).

### GDT and TSS

`kernel/arch/x86_64/gdt.c` manages a 7-entry GDT: null, kernel code (0x08), kernel data (0x10), TSS low (0x18), TSS high (0x20), user data (0x2b, DPL=3), user code 64 (0x33, DPL=3). Selectors exported as `GDT_KERNEL_CODE`, `GDT_KERNEL_DATA`, `GDT_USER_DATA`, `GDT_USER_CODE_64` in `gdt.h`. `tss_set_rsp0()` in `tss.c` updates `tss.rsp[0]` dynamically before entering a user thread.

### SYSCALL/SYSRET

`syscall_init()` (`kernel/syscall/syscall.c`) sets EFER.SCE, STAR `(0x0020<<48)|(0x0008<<32)`, LSTAR = `syscall_entry`, FMASK = 0x200 (mask IF). STAR encoding: SYSCALL → CS=0x08/SS=0x10; SYSRET → SS=0x2b/CS=0x33.

`kernel/arch/x86_64/syscall_entry.asm`: `swapgs` to access per-CPU data (`cpu_local.kernel_rsp` at GS:0, `cpu_local.user_rsp` at GS:8), swaps to kernel stack, saves registers, calls `syscall_dispatch`, restores, `o64 sysret`.

Per-CPU data (`kernel/arch/x86_64/cpu.c/h`): `struct cpu_local { kernel_rsp, user_rsp }` array indexed by LAPIC ID. `cpu_local_init(lapic_id)` writes `IA32_KERNEL_GS_BASE` (0xC0000102). Called from `sched_init` (BSP) and `sched_ap_enter` (APs).

Syscalls (Linux ABI: nr in RAX, args in RDI/RSI/RDX/R10/R8/R9): `sys_write` (nr=1, fd=1 → serial), `sys_exit` (nr=60).

### ELF64 Loader and Per-Process Address Spaces

`kernel/exec/elf.c`: `elf_load(image, size, result)` validates ELF64 ET_EXEC for EM_X86_64, calls `vmm_space_create()`, maps each PT_LOAD segment with `VMM_USER` flags, allocates 4-page user stack at `0x7fffffffe000`–`0x7fffffffffff`. Returns entry, user_sp, cr3.

`kernel/mem/vmm.c` address space API:
- `vmm_space_create()`: allocates PML4, zeroes user half, copies kernel PML4 entries [256-511] so kernel is visible in every process.
- `vmm_space_map(pml4_phys, virt, phys, flags)`: maps a single page in a specific address space.
- `vmm_space_switch(pml4_phys)`: writes CR3.
- `vmm_space_destroy(pml4_phys)`: stub for M3 (no page-table walk yet).

`kernel/proc/process.c`: `process_create_from_elf()` calls `elf_load`, then `thread_create_user(entry, user_sp, cr3)`.

### User Threads and Context Switch

`thread_create_user(entry_rip, user_rsp, cr3)` in `thread.c`: same 20-qword iretq frame as kernel threads but with `CS=0x33`, `SS=0x2b`, `RSP=user_rsp`, `RFLAGS=0x202`. When the timer ISR preempts the user thread, the hardware pushes the user frame onto the kernel stack (TSS rsp0 points to the thread's kernel stack top). `sched_tick()` updates `tss_set_rsp0(next->kstack_top)` and calls `vmm_space_switch(next->cr3)` before returning to the scheduler for user threads.

`struct thread` now includes `kstack_top`, `is_user`, and `cr3` fields.

### First User Process

`user/hello/hello.asm`: NASM ELF64 binary linked at 0x400000. Calls `sys_write(1, "Hello from userspace!\n", 22)` then `sys_exit(0)`. Built by the Makefile as `build/hello.elf` and embedded via `user/hello/hello_blob.asm` (`incbin`). Launched from `kernel_main` after `ktest_run_all()`.

### Test Harness

`kernel/test/ktest.c` provides `ktest_suite`, `ktest_check`, and the `KTEST_ASSERT` macro. Results go to serial. `kernel/test/tests.c:ktest_run_all()` calls all suites and panics if any assertion failed. Add new suites by creating `test_*.c`, declaring the run function in `tests.h`, calling it from `tests.c`, and adding the `.c` file to `KERNEL_C_SRCS` in `Makefile`.

## Code Conventions

- C11, `-ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -mgeneral-regs-only`.
- No libc. String utilities are in `kernel/lib/string.c`. Serial I/O via `kernel/core/serial.c`.
- Shared include path: `include/` (bootloader+kernel shared headers). Kernel-only headers: `kernel/` (use `-Ikernel`).
- Panic unconditionally halts: `kernel/core/panic.c` prints to serial then executes `hlt` in a loop.
- Assembly files use NASM syntax (`-f elf64`). The `.note.GNU-stack` section must be present to suppress linker warnings.
