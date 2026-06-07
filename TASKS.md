# Tasks

## Current Work

- Keep Phase 1 boot verification stable and documented.
- Keep every incomplete requirement visible instead of marking placeholders as
  complete.

## Completed

- Created initial repository structure.
- Added freestanding build rules.
- Added documentation baseline.
- Added UEFI loader implementation.
- Added higher-half kernel entry and early kernel infrastructure.
- Added ESP staging and ISO generation script.
- Installed local runtime/image dependencies: QEMU, OVMF, xorriso, and mtools.
- Verified `BOOTX64.EFI` under QEMU/OVMF using the staged ESP path.
- Verified `build/os.iso` under QEMU/OVMF.
- Added automated QEMU boot smoke test.
- Milestone 1: PMM (full UEFI type policy), VMM (map/unmap/protect), heap, LAPIC timer, IOAPIC, ktest harness.
- Milestone 2: ACPI MADT parsing, SMP AP startup, preemptive round-robin scheduler, kernel threads, spinlock and mutex.
- Milestone 3: Ring 3 transition, SYSCALL/SYSRET ABI, ELF64 loader, per-process address spaces, process lifecycle, first user process.

## Open Implementation Tasks

- Milestone 4: Storage (PCI enumeration, AHCI/NVMe, GPT, VFS, ext2).
- Keyboard driver (PS/2 or USB HID) for interactive shell.
- Validate ISO on real UEFI hardware.

## Blockers

- Real UEFI hardware validation has not been performed.

## Technical Debt

- Early direct map is limited to the first 64 GiB.
- PMM currently tracks only the first 64 GiB.
- Bootloader page tables intentionally keep identity mappings for early bring-up.
