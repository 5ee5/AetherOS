# AetherOS

A hobby x86-64 operating system written from scratch in C and NASM assembly. Boots via UEFI, runs on real hardware and QEMU, and supports multiple CPU cores, preemptive multitasking, per-process address spaces, a writable ext2 filesystem, an in-kernel TCP/IP stack, multi-user login with file permissions, and an interactive shell with pipes, job control, and 30+ real user-space programs.

## What it does

**Boots from UEFI firmware on real hardware and QEMU** — a custom PE32+ EFI application loads the kernel ELF from the ESP partition, builds 4-level page tables, captures GOP framebuffer / ACPI / memory map, calls `ExitBootServices`, and jumps to the kernel in the higher half (`0xffffffff80000000`). A single 64 MiB USB image (`make usb`) can be flashed to any thumb drive and booted on a real UEFI PC.

**Manages physical and virtual memory** — a bitmap PMM tracks every 4 KiB page across the first 64 GiB of RAM. The VMM walks 4-level page tables for `vmm_map`/`vmm_unmap`/`vmm_protect`, and each process gets its own PML4 address space with the kernel half shared across all of them.

**Runs multiple CPU cores (SMP)** — a flat-binary trampoline at physical `0x8000` brings APs from real mode → protected mode → 64-bit mode, then drops them into the scheduler's idle loop. Tested with up to 4 cores under QEMU.

**Preemptive round-robin scheduler** — the APIC timer fires at ~1 kHz. The timer ISR saves all 15 GP registers, calls the C scheduler to pick the next thread, and `iretq`s into it. Threads get 64 KiB kernel stacks; user threads get separate user stacks and switch address spaces on context switch.

**User-space processes** — ELF64 executables are loaded into per-process address spaces (ring 3). System calls go through `SYSCALL`/`SYSRET` with `swapgs` for per-CPU kernel stack switching. All user-pointer access goes through fault-recovering `copy_to/from_user` helpers, so a bad pointer returns `-EFAULT` instead of faulting the kernel. Syscalls span the file/process/socket surface: `read`, `write`, `open` (O_CREAT/O_TRUNC/O_APPEND), `close`, `stat`, `pipe`, `creat`, `mkdir`, `unlink`, `rename`, `chdir`, `getcwd`, `listdir`, `chmod`, `chown`, `exit`, `waitpid`, `getpid`, `kill`, `sleep`, `spawn`, `spawn_as`, `ps`, `meminfo`, `diskinfo`, `reboot`, the credential calls (`getuid`/`getgid`/`geteuid`/`getegid`/`setuid`/`setgid`/`seteuid`/`setegid`), and the network calls (`socket`, `connect`, `send`, `recv`, `dns_resolve`).

**Writable ext2 filesystem** — full read/write ext2 support including block/inode allocation, directory mutation (`create`, `mkdir`, `unlink`), and file truncation. The VFS layer exposes all operations to user processes. Data is persisted in a GPT disk image via an AHCI driver.

**Multi-user login & permissions** — boots to a `login` prompt that authenticates against `/etc/passwd` and starts the shell as the logged-in user. Processes carry real/effective/saved UID & GID credentials; the ext2 layer enforces rwx permission bits, and the setuid bit on an executable elevates privilege on exec. `sudo`, `passwd`, `useradd`, and `userdel` manage users and run commands as root.

**Interactive shell** — runs in ring 3, supports built-in commands (`cd`, `pwd`, `ls`, `cat`, `echo`, `clear`, `uname`, `help`, `history`, `exit`) and spawns ELF binaries from the filesystem. Adds **pipelines (up to 4 stages), background jobs (`&`), Ctrl+C interrupt, `>` output redirection, and command history with up/down arrow recall**. The prompt shows the current working directory; relative paths are resolved against it.

**tinylibc** — a minimal C library (`crt0`, `printf` with width specifiers, `malloc`/`free`/`calloc`/`realloc` with a free-list allocator, string functions, syscall wrappers) that lets user programs be written in C without any host libc dependency.

**Desktop GUI** — a graphical desktop runs on the GOP linear framebuffer with an 8×16 bitmap font, a desktop compositor, floating windows, a clock, and a live kernel log window that mirrors serial output.

**Networking** — e1000 NIC driver, ARP, DHCP, IPv4, ICMP, UDP, DNS, TCP — implemented in-kernel and exposed to userland via socket syscalls (`socket`, `connect`, `send`, `recv`, `dns_resolve`). `/bin/wget` performs real HTTP GET requests over TCP.

## Features at a glance

| Area | Detail |
|---|---|
| Boot | UEFI (BOOTX64.EFI), real hardware + QEMU, higher-half kernel, GOP framebuffer |
| CPU | x86-64, SMP (up to 64 cores via LAPIC IDs) |
| Memory | Bitmap PMM, 4-level VMM, first-fit heap with coalescing |
| Scheduling | Preemptive round-robin, APIC timer ~1 kHz |
| Processes | Per-process PML4, ring-3 user mode, SYSCALL/SYSRET |
| Filesystem | GPT + ext2 (read+write) over AHCI (SATA), basic NVMe, VFS abstraction |
| Networking | e1000 NIC, ARP, DHCP, IPv4, ICMP, UDP, DNS, TCP — socket syscalls exposed to userland |
| GUI | Framebuffer desktop, 8×16 font, windows, live kernel log |
| Shell | built-ins + pipes (4 stages), `&` background jobs, Ctrl+C, `>`, history, ELF programs |
| Userland | 33 programs via tinylibc (coreutils, users, net, sysinfo — see below) |
| Sync | Spinlocks (incl. IRQ-safe), mutexes with wait queues; SMP-safe heap/PMM/VFS/sockets/pipes |
| ACPI | RSDP → RSDT/XSDT → MADT parsing for LAPIC/IOAPIC discovery; ACPI poweroff/reboot |
| Input | PS/2 keyboard driver with IRQ-driven scancode handling |
| Security | Real/effective/**saved** UID & GID, ext2 permission enforcement, setuid-on-exec, fault-recovering `copy_to/from_user` |
| Multi-user | `login` against `/etc/passwd`, `sudo`/`passwd`/`useradd`/`userdel`, per-user shells |

## Building

**Requirements:** `gcc`, `nasm`, `ld`, `xorriso`, `mtools`, `parted`, `debugfs` (e2fsprogs), `qemu-system-x86_64`, OVMF firmware.

```sh
./scripts/check-deps.sh   # verify tools
make                      # build bootloader + kernel ELF → build/esp/
make disk                 # create build/disk.img with user programs
make iso                  # produce build/os.iso
make test                 # boot smoke test via QEMU (non-interactive)
make run-qemu             # interactive QEMU session, keyboard via serial stdio
```

## Running

```sh
make run-qemu
```

QEMU launches with `-serial stdio` and `-display none`. The system boots to a login prompt — sign in as **`root`** / **`root`** — then type commands directly in the terminal:

```
AetherOS login: root
Password:
Welcome to AetherOS, root!
/root # whoami
root
/root # id
uid=0(root) gid=0(root)
/root # ls /bin
cat  chmod  chown  cp  df  grep  ls  mkdir  mv  ps  rm  sudo  wc  wget  whoami  ...
/root # cd /bin && ls cat            # relative paths resolve against cwd
cat
/bin # echo hello world > /tmp.txt
/bin # cat /tmp.txt
hello world
/bin # ps
  PID   UID  NAME
    1     0  login
    2     0  shell
/bin # free
/bin # cat /etc/passwd | grep root   # pipelines work
/bin # wget http://example.com/
Resolving example.com...
Connecting to 93.184.216.34:80...
<!doctype html>...
/bin # exit
```

A graphical desktop also renders on the GOP framebuffer — run with `-display gtk` (instead of `-display none`) to see it.

## Running on real hardware

Build a single bootable USB image and flash it to a drive:

```sh
make usb                        # produces build/usb.img (64 MiB)
make flash DISK=/dev/sdX        # writes to USB drive — destructive
```

Or write manually:

```sh
dd if=build/usb.img of=/dev/sdX bs=4M status=progress oflag=sync
```

The image contains two GPT partitions: an ext2 data partition (filesystem + user programs) and a FAT32 EFI System Partition (bootloader + kernel ELF). Plug into any UEFI machine, disable Secure Boot, and select the USB from the boot menu.

Pre-built USB images are attached to every [GitHub release](https://github.com/5ee5/AetherOS/releases) as `aetheros-vX.Y.Z-usb.img.gz`.

## Repository layout

```
boot/uefi/       UEFI bootloader (C)
kernel/
  arch/x86_64/   GDT, IDT, APIC, SMP, syscall entry (C + NASM)
  mem/           PMM, VMM, heap
  sched/         scheduler, threads
  proc/          processes, file descriptors
  exec/          ELF loader
  fs/            VFS, ext2 (read+write)
  net/           e1000, ARP/DHCP/IPv4/ICMP/UDP/TCP/DNS
  gui/           framebuffer, font, windows, desktop
  syscall/       syscall dispatch
  security/      process credentials (real/effective/saved UID & GID)
  drivers/       AHCI, NVMe, PCI, PS/2 keyboard
  test/          in-kernel test suites (8 suites)
user/
  libc/          tinylibc (crt0, printf, free-list malloc, syscall wrappers)
  bin/           33 programs (C source): coreutils (ls, cat, cp, mv, rm,
                 mkdir, touch, wc, head, tail, sort, grep, find, stat,
                 chmod, chown), users (login, sudo, passwd, useradd,
                 userdel, whoami, id), net (wget), sysinfo (ps, df, free,
                 uname), power (poweroff, reboot, kill, sleep, pwd)
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
- ext2 superblock cached in `ext2_fs_t` for O(1) allocation counters; block/inode bitmaps are read-modify-write on each alloc/free
- User-memory access uses `copy_to/from_user` backed by an exception table: a fault on a user pointer is trapped via a fixup entry and returned as `-EFAULT` rather than panicking the kernel
- SMP-shared state (heap, PMM, VFS open-file table, sockets, pipes) is spinlock-protected; the heap and PMM use IRQ-safe locks because they are reached from the timer ISR's deferred thread reaper
