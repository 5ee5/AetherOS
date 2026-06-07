# Roadmap

## Milestone 0: Repository And Boot Foundation

- Status: complete for the QEMU/OVMF target.
- Goal: produce a reproducible build that reaches a higher-half kernel through a
  custom UEFI loader.

Implemented in this milestone:

- Freestanding build structure.
- Custom UEFI loader source.
- Boot protocol shared by loader and kernel.
- Higher-half kernel linker layout.
- Early serial logging, panic path, GDT, IDT, exception dispatch, PMM bootstrap,
  and VMM page-table walker.
- ESP tree generation and bootable ISO generation.
- QEMU/OVMF boot validation for both the staged ESP helper path and `build/os.iso`.
- Automated serial-output smoke test gated on `kernel: initialization complete`.

Remaining before claiming broader platform support:

- Validate the ISO on real UEFI hardware.
- Add stronger negative-path bootloader tests for malformed kernel images.

## Milestone 1: Core Kernel Correctness

Status: complete.

- Physical memory manager with full UEFI memory-type policy.
- Virtual memory manager with map/unmap/protect APIs.
- Kernel heap allocator (first-fit, coalescing, backed by PMM).
- APIC timer and local APIC initialization (PIT-calibrated ~1 kHz).
- IOAPIC routing (all 24 entries masked at init).
- Kernel test harness (ktest_suite / KTEST_ASSERT, run at every boot).

## Milestone 2: SMP And Scheduling

Status: complete.

- ACPI MADT parsing (LAPIC base, CPU LAPIC IDs, IOAPIC entry; RSDT and XSDT).
- SMP application processor startup (flat-binary trampoline at 0x8000, 16→32→64-bit, INIT+SIPI sequence).
- Preemptive round-robin scheduler (timer vector 0x20, per-CPU run queue, full GP register context switch).
- Kernel threads (20-qword initial stack frame, thread_create / thread_exit / thread_yield).
- Spinlock (xchg-based with pause hint) and mutex (spinlock + wait queue, sleeping).

## Milestone 3: Userspace

Status: complete.

- Ring 3 transition (GDT user selectors, iretq to ring-3 thread via existing scheduler).
- SYSCALL/SYSRET ABI (EFER.SCE, STAR/LSTAR/FMASK, swapgs + per-CPU data for stack swap).
- ELF64 loader (validates header, loads PT_LOAD segments into fresh PML4, maps user stack).
- Per-process address spaces (vmm_space_create/map/switch/destroy; kernel half shared via PML4 copy).
- Process lifecycle (process_create_from_elf, sys_exit, sys_write → serial).
- Embedded first user process: prints "Hello from userspace!" via sys_write then sys_exit.

## Milestone 4: Storage

- PCI enumeration.
- AHCI and/or NVMe block device.
- GPT partition discovery.
- VFS.
- ext2 read/write.
- File permissions and ownership.

## Milestone 5: Network, Security, And Desktop

- Network drivers and IPv4/UDP/TCP.
- DHCP and DNS.
- User accounts, auditing, and access control.
- Framebuffer compositor and native GUI shell.
