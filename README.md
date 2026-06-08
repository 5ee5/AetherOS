# AetherOS

A hobby x86-64 operating system written from scratch in C and NASM assembly. Boots via UEFI, runs on real hardware and QEMU, and supports multiple CPU cores, preemptive multitasking, a virtual filesystem, and an interactive shell with real user-space programs.

## What it does

**Boots from UEFI firmware** — a custom PE32+ EFI application loads the kernel ELF from the ESP partition, builds 4-level page tables, captures GOP framebuffer / ACPI / memory map, calls `ExitBootServices`, and jumps to the kernel in the higher half (`0xffffffff80000000`).

**Manages physical and virtual memory** — a bitmap PMM tracks every 4 KiB page across the first 64 GiB of RAM. The VMM walks 4-level page tables for `vmm_map`/`vmm_unmap`/`vmm_protect`, and each process gets its own PML4 address space with the kernel half shared across all of them.

**Runs multiple CPU cores (SMP)** — a flat-binary trampoline at physical `0x8000` brings APs from real mode → protected mode → 64-bit mode, then drops them into the scheduler's idle loop. Tested with up to 4 cores under QEMU.

**Preemptive round-robin scheduler** — the APIC timer fires at ~1 kHz. The timer ISR saves all 15 GP registers, calls the C scheduler to pick the next thread, and `iretq`s into it. Threads get 64 KiB kernel stacks; user threads get separate user stacks and switch address spaces on context switch.

**User-space processes** — ELF64 executables are loaded into per-process address spaces (ring 3). System calls go through `SYSCALL`/`SYSRET` with `swapgs` for per-CPU kernel stack switching. Implemented syscalls cover `read`, `write`, `open`, `close`, `exit`, `waitpid`, `getpid`, `spawn`, and `listdir`.

**ext2 filesystem** — the kernel reads an ext2 partition (GPT disk image) via an AHCI driver. The VFS layer exposes `open`/`read`/`close`/`listdir` to user processes. Files on the disk are accessible from the shell.

**Interactive serial shell** — the shell process runs in ring 3, reads keyboard input through the COM1 serial port (works with `qemu -serial stdio`), and supports built-in commands plus spawning ELF binaries from the filesystem with arguments.

**tinylibc** — a minimal C library (`crt0`, `printf`, `malloc`, string functions, syscall wrappers) that lets user programs be written in C without any host libc dependency.

## Features at a glance

| Area | Detail |
|---|---|
| Boot | UEFI (BOOTX64.EFI), higher-half kernel, GOP framebuffer |
| CPU | x86-64, SMP (up to 64 cores via LAPIC IDs) |
| Memory | Bitmap PMM, 4-level VMM, first-fit heap |
| Scheduling | Preemptive round-robin, APIC timer ~1 kHz |
| Processes | Per-process PML4, ring-3 user mode, SYSCALL/SYSRET |
| Filesystem | GPT + ext2 over AHCI (SATA), VFS abstraction |
| Networking | e1000 NIC driver, ARP, DHCP, IPv4, ICMP, UDP, TCP (basic) |
| GUI | Linear framebuffer, 8×16 bitmap font, desktop + windows |
| Shell | Interactive serial shell, built-ins + ext2-resident ELF programs |
| Userland | `ls`, `cat`, `wc`, `uname` as real C ELF binaries via tinylibc |
| Sync | Spinlocks, mutexes with wait queues |

## Building

**Requirements:** `gcc`, `nasm`, `ld`, `xorriso`, `mtools`, `parted`, `debugfs` (e2fsprogs), `qemu-system-x86_64`, OVMF firmware.

```sh
./scripts/check-deps.sh   # verify tools
make                      # build bootloader + kernel ELF → build/esp/
make disk                 # create build/disk.img with /bin/{ls,cat,wc,uname}
make iso                  # produce build/os.iso
make test                 # boot smoke test via QEMU (non-interactive)
make run-qemu             # interactive QEMU session, keyboard via serial stdio
```

## Running

```sh
make run-qemu
```

QEMU launches with `-serial stdio` and `-display none`. Type commands directly in the terminal:

```
AetherOS shell v0.6 — type 'help' for commands
$ help
$ ls /bin
$ /bin/uname
$ /bin/cat /hello.txt
$ /bin/wc /hello.txt
$ /bin/ls /
$ echo hello world
$ exit
```

## Repository layout

```
boot/uefi/       UEFI bootloader (C)
kernel/
  arch/x86_64/   GDT, IDT, APIC, SMP, syscall entry (C + NASM)
  mem/           PMM, VMM, heap
  sched/         scheduler, threads
  proc/          processes, file descriptors
  exec/          ELF loader
  fs/            VFS, ext2
  net/           e1000, ARP/DHCP/IPv4/ICMP/UDP/TCP
  gui/           framebuffer, font, windows, desktop
  syscall/       syscall dispatch
  drivers/       AHCI, PCI, PS/2 keyboard
  test/          in-kernel test suites
user/
  libc/          tinylibc (crt0, printf, malloc, string, syscall wrappers)
  bin/           ls, cat, wc, uname (C source)
  shell/         interactive shell (NASM)
  hello/         minimal hello-world ELF (NASM)
include/os/      shared bootloader↔kernel headers (bootinfo, ELF64)
scripts/         build helpers (QEMU, ISO, disk image, dep check)
```

## Architecture notes

- Kernel linked at `-mcmodel=kernel` with virtual base `0xffffffff80000000`
- Higher-half direct map at `0xffff800000000000` covering 64 GiB physical
- Heap at `0xffff900000000000`, LAPIC at `0xffffa00000000000`
- Stack guard page (4 KiB, unmapped) below the 64 KiB kernel stack catches overflows
- AP idle threads pre-allocated on the BSP before SIPI to avoid allocation races
- User processes share kernel PML4 entries [256–511]; per-process entries [0–255]
- `sys_spawn` copies argv from caller's address space, switches to child CR3, writes the x86-64 ABI initial stack (argc + argv[] + strings), then switches back before thread creation
