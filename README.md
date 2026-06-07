# OS

This repository contains a from-scratch x86_64 operating system targeting UEFI
systems. The project is intentionally staged: features are only marked complete
after they are implemented and have a build or runtime verification path.

## Current State

The current milestone provides:

- A freestanding build system for a UEFI bootloader and higher-half kernel.
- A custom UEFI loader that loads `EFI/OS/KERNEL.ELF`, builds early paging, exits
  boot services, and transfers control to the kernel.
- Early kernel infrastructure: serial logging, GDT/IDT setup, exception stubs,
  physical-memory bitmap initialization, active page-table walking, and panic
  handling.
- A QEMU/OVMF-verified bootable ISO path with a serial-output smoke test.

This is not yet a complete operating system. See `ROADMAP.md`, `TASKS.md`, and
`KNOWN_LIMITATIONS.md` for the active scope and gaps.

## Required Tools

Minimum local build tools:

- `gcc`
- `ld`
- `nasm`
- `objcopy`
- `make`

Runtime/image validation tools:

- `qemu-system-x86_64`
- OVMF firmware
- `xorriso`
- `mtools`

Run:

```sh
./scripts/check-deps.sh
```

## Build

```sh
make all
```

The build produces:

- `build/kernel/kernel.elf`
- `build/BOOTX64.EFI`
- `build/esp/EFI/BOOT/BOOTX64.EFI`
- `build/esp/EFI/OS/KERNEL.ELF`

Build a UEFI optical image when `xorriso` and `mtools` are available:

```sh
make iso
```

Run the boot smoke test:

```sh
make test
```

The smoke test boots `build/os.iso` in QEMU/OVMF and passes only after the serial
log contains `kernel: initialization complete`.

## Run In QEMU

After installing QEMU and OVMF:

```sh
./scripts/run-qemu.sh
```

The kernel logs to COM1 serial output.
