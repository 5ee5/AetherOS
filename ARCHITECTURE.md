# Architecture

## Overview

The operating system targets x86_64 UEFI systems. The initial architecture is a
monolithic kernel with explicit subsystem boundaries. This reduces early
driver/scheduler/IPC complexity while keeping a path open for stronger
privilege separation in later userspace services.

## Boot Flow

1. Firmware loads `EFI/BOOT/BOOTX64.EFI`.
2. The custom UEFI loader opens `EFI/OS/KERNEL.ELF` from the same device.
3. The loader validates the ELF64 image and loads each `PT_LOAD` segment to its
   physical load address.
4. The loader builds early 4-level paging:
   - identity mapping for the first 64 GiB;
   - higher-half direct map at `0xffff800000000000`;
   - kernel mappings at `0xffffffff80000000`.
5. The loader captures GOP framebuffer, ACPI RSDP, and final UEFI memory map.
6. The loader exits boot services, switches to the new page tables, and jumps to
   the higher-half kernel entry point.

## Boot Image Format

The build emits a PE32+ EFI application with conventional 4 KiB-aligned section
RVAs and a minimal relocation directory. The ISO builder embeds a small FAT EFI
System Partition image for El Torito boot and also mirrors `/EFI/BOOT` and
`/EFI/OS` into the ISO filesystem for firmware fallback paths.

The automated boot smoke test boots `build/os.iso` in QEMU/OVMF and requires the
kernel serial marker `kernel: initialization complete`.

## Kernel Address Layout

- Kernel virtual base: `0xffffffff80000000`.
- Kernel physical load base: `0x00200000`.
- Higher-half direct map base: `0xffff800000000000`.
- Early direct map size: 64 GiB.

## Early Kernel Subsystems

- Serial logger: 16550-compatible COM1 output for deterministic diagnostics.
- GDT: installs kernel code and data descriptors for long mode.
- IDT: installs exception gates for vectors 0-31.
- PMM: initializes a bitmap from the UEFI memory map and exposes page
  allocation/free primitives.
- VMM: walks the active 4-level page tables and translates virtual addresses.

## Security Posture

Current security guarantees are early boot correctness only. Ring 3 isolation,
syscall validation, process isolation, credential storage, auditing, and secure
boot compatibility are not implemented yet. The bootloader does not yet verify
kernel signatures.
