# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```sh
make              # build ESP tree (bootloader + kernel ELF)
make disk         # create build/disk.img with user programs (QEMU data disk)
make usb          # create build/usb.img (64 MiB bootable USB image for real hardware)
make flash DISK=/dev/sdX  # write usb.img to a USB drive (destructive)
make iso          # produce build/os.iso (requires xorriso + mtools)
make test         # boot smoke test via QEMU/OVMF (requires iso)
make run-qemu     # interactive QEMU session, serial on stdout
make clean        # wipe build/
./scripts/check-deps.sh   # verify required tools are present
```

There is no single-test target. The kernel test harness runs all suites at boot via `ktest_run_all()` in `kernel/kernel.c`. Test output goes to COM1 serial. The smoke test (`make test`) fails if `ktest: FAIL` appears or `ktest:` output is absent.

## Release Workflow

`.github/workflows/release.yml` auto-creates a GitHub release on every push to `master`. It bumps the version based on commit message content:

- Default (no tag): patch bump (`v1.1.1 → v1.1.2`)
- Subject contains `[minor]` as a standalone token: minor bump (`v1.1.x → v1.2.0`)
- Subject contains `[major]` as a standalone token: major bump (`v1.x.x → v2.0.0`)

Release artifacts: `esp.tar.gz`, `kernel.elf`, `BOOTX64.EFI`, `usb.img.gz`.

**Do not add `[minor]` or `[major]` anywhere in commit message text that isn't meant as a version bump tag** — the detector scans commit subjects for these exact tokens.

## Architecture

### Boot Flow

UEFI firmware → `build/BOOTX64.EFI` (PE32+ EFI app, built from `boot/uefi/main.c`) → loads `EFI/OS/KERNEL.ELF` from the same device, validates ELF64, maps PT_LOAD segments, builds 4-level paging, captures GOP/RSDP/memory map, calls `ExitBootServices`, switches to new CR3, jumps to `kernel_entry` in the higher half.

`include/os/bootinfo.h` defines `struct os_boot_info` — the only ABI contract between bootloader and kernel.

### USB Boot Image (`scripts/make-usb.sh`)

Single 64 MiB image with two GPT partitions:
- Partition 0 (LBA 2048–67583): ext2 data — kernel mounts this as root via `gpt_read()` → `parts[0]`
- Partition 1 (LBA 67584–end): FAT32 ESP — UEFI finds by type GUID, contains `BOOTX64.EFI` + `KERNEL.ELF`

UEFI finds the ESP by partition type GUID (not index), so putting ext2 first in the GPT entry table is safe and requires zero kernel changes. `kernel/fs/vfs.c` always mounts `parts[0]`.

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

`serial_init` → validate boot info → `gdt_init` → `tss_init` → `syscall_init` → `idt_init` → `fb_init` → `pmm_init` → `vmm_init` → unmap stack guard → `heap_init` → `acpi_init` → `lapic_init` + `apic_timer_init` → `ioapic_init` → `sched_init` → `smp_init` → `ktest_run_all` → try `vfs_open("/bin/login")` (fallback: embedded shell blob) → `process_create_from_elf` → idle loop.

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

Syscalls (Linux ABI: nr in RAX, args in RDI/RSI/RDX/R10/R8/R9):

| nr | Name | Notes |
|---|---|---|
| 0 | `sys_read` | fd=0 → PS/2 keyboard; fd>2 → file or socket |
| 1 | `sys_write` | fd=1/2 → serial; fd>2 → file or socket |
| 2 | `sys_open` | supports O_CREAT, O_TRUNC, O_APPEND |
| 3 | `sys_close` | closes file or socket fd |
| 4 | `sys_stat` | path + `struct stat*`; fills mode, nlink, uid, gid, size, mtime |
| 22 | `sys_pipe` | creates pipe; fills int[2] with read/write fds |
| 35 | `sys_sleep` | custom: sleeps for `a0` milliseconds (busy-wait) |
| 39 | `sys_getpid` | |
| 41 | `sys_socket` | AF_INET, SOCK_STREAM only |
| 42 | `sys_connect` | takes `struct sockaddr_in*` |
| 44 | `sys_send` | |
| 45 | `sys_recv` | |
| 60 | `sys_exit` | |
| 61 | `sys_waitpid` | |
| 62 | `sys_kill` | sends SIGKILL to pid (terminates process) |
| 79 | `sys_getcwd` | |
| 80 | `sys_chdir` | |
| 82 | `sys_rename` | rename/move file or directory |
| 83 | `sys_mkdir` | |
| 87 | `sys_unlink` | |
| 90 | `sys_chmod` | change permission bits |
| 92 | `sys_chown` | change uid/gid of a file; root-only to change to arbitrary uid |
| 102 | `sys_getuid` | returns current process uid |
| 104 | `sys_getgid` | returns current process gid |
| 105 | `sys_setuid` | sets uid/euid; root-only to change to arbitrary uid |
| 106 | `sys_setgid` | sets gid/egid |
| 107 | `sys_geteuid` | returns effective uid |
| 108 | `sys_getegid` | returns effective gid |
| 500 | `sys_spawn` | custom: path + argv[], inherits parent creds, returns pid |
| 501 | `sys_spawn_as` | custom: root-only; spawn with explicit uid/gid (used by login) |
| 600 | `sys_listdir` | custom: fills buffer with null-separated names |
| 601 | `sys_creat` | custom: create file |
| 602 | `sys_dns` | custom: resolve hostname → IPv4 |
| 603 | `sys_ps` | custom: fills buffer with process list (pid name uid lines) |
| 604 | `sys_meminfo` | custom: writes `uint64_t[2]` {total_bytes, free_bytes} to user ptr |
| 605 | `sys_diskinfo` | custom: writes `uint64_t[2]` {total_bytes, free_bytes} to user ptr |

### File Descriptor Table (`kernel/proc/fd.c/h`)

Each process has a `fd_table_t` with entries of type `fd_entry_t { bool open; fd_type_t type; int id; }`. `fd_type_t` is `FD_NONE`, `FD_FILE`, `FD_SOCKET`, `FD_PIPE_READ`, or `FD_PIPE_WRITE`. `fd_to_vfs()` returns the VFS fd for file entries. `fd_alloc_socket()` allocates a socket fd. `fd_type()` / `fd_id()` query an entry.

### Networking (`kernel/net/`)

- **e1000** (`drivers/e1000.c`): Intel e1000 NIC driver. Registers as the global NIC via `nic_register()`.
- **Stack**: `eth.c` → `arp.c` / `ipv4.c` → `icmp.c` / `udp.c` / `tcp.c`
- **DHCP** (`net/dhcp.c`): obtains IP at init via broadcast discover/offer/request/ack.
- **DNS** (`net/dns.c`): single UDP query to DHCP-provided nameserver.
- **TCP** (`net/tcp.c`): active-open client only. `tcp_connect()` blocks (polling) until ESTABLISHED or timeout. `tcp_recv()` returns 0 for empty buffer; returns 0 and state≠ESTABLISHED when connection closed (FIN received). `tcp_is_established()` predicate exposed in `tcp.h`.
- **Socket table** (`net/socket.c`): 8-slot array of `tcp_conn_t*`. `sock_recv()` returns -1 on EOF (connection closed + empty buffer). `sys_read`/`sys_recv` loops exit on -1 and return 0 bytes to userland.

### VFS and ext2 (`kernel/fs/`)

`kernel/fs/vfs.c`: mounts first GPT partition as ext2 root. Exposes `vfs_open`, `vfs_open_ex` (O_CREAT/O_TRUNC/O_APPEND), `vfs_read`, `vfs_write`, `vfs_close`, `vfs_listdir`, `vfs_creat`, `vfs_mkdir`, `vfs_unlink`, `vfs_chdir`, `vfs_getcwd`, `vfs_rename`, `vfs_stat`, `vfs_chown`, `vfs_chmod`, `vfs_disk_stats`.

`kernel/fs/ext2.c`: full read/write ext2. Supports 12 direct blocks + singly-indirect + doubly-indirect (~64 MiB max file size with 1024-byte blocks). Block/inode bitmap alloc, BGD updates, directory mutation (`dir_add_entry`, `dir_remove_entry`). `ext2_mount`, `ext2_read`, `ext2_write`, `ext2_create`, `ext2_mkdir`, `ext2_unlink`, `ext2_truncate`, `ext2_rename`, `ext2_disk_stats`, `ext2_stat_full`, `ext2_inode_chown`, `ext2_chmod`.

Per-process working directory stored as `char cwd[256]` in `struct process` (`kernel/proc/process.h`), initialized to `"/"`.

### ELF64 Loader and Per-Process Address Spaces

`kernel/exec/elf.c`: `elf_load(image, size, result)` validates ELF64 ET_EXEC for EM_X86_64, calls `vmm_space_create()`, maps each PT_LOAD segment with `VMM_USER` flags, allocates 4-page user stack at `0x7fffffffe000`–`0x7fffffffffff`. Returns entry, user_sp, cr3.

`kernel/mem/vmm.c` address space API:
- `vmm_space_create()`: allocates PML4, zeroes user half, copies kernel PML4 entries [256-511] so kernel is visible in every process.
- `vmm_space_map(pml4_phys, virt, phys, flags)`: maps a single page in a specific address space.
- `vmm_space_switch(pml4_phys)`: writes CR3.
- `vmm_space_destroy(pml4_phys)`: stub (no page-table walk yet).

`kernel/proc/process.c`: `process_create_from_elf()` calls `elf_load`, then `thread_create_user(entry, user_sp, cr3)`.

### User Threads and Context Switch

`thread_create_user(entry_rip, user_rsp, cr3)` in `thread.c`: same 20-qword iretq frame as kernel threads but with `CS=0x33`, `SS=0x2b`, `RSP=user_rsp`, `RFLAGS=0x202`. When the timer ISR preempts the user thread, the hardware pushes the user frame onto the kernel stack (TSS rsp0 points to the thread's kernel stack top). `sched_tick()` updates `tss_set_rsp0(next->kstack_top)` and calls `vmm_space_switch(next->cr3)` before returning to the scheduler for user threads.

`struct thread` includes `kstack_top`, `is_user`, and `cr3` fields.

### Shell (`user/shell/shell.asm`)

NASM ELF64 shell linked at 0x400000. Built-in commands: `echo`, `cat`, `ls`, `cd`, `pwd`, `clear`, `uname`, `help`, `exit`, `history`. Spawns ELF binaries from the filesystem. Prompt shows cwd. Echo supports `>` output redirection. Up/down arrow keys navigate command history (ring buffer of recent commands).

Arg splitting: `.try_spawn` tokenizes the remaining line by spaces, null-terminating each token in `line_buf`, building up to 8 argv pointers in `spawn_argv` (10 qwords). Programs receive properly split `argv[]`.

### tinylibc (`user/libc/`)

Minimal C library for user programs. No host libc dependency.
- `src/crt0.asm`: entry point, calls `main`, calls `exit`
- `src/syscall.c`: `__sc1`–`__sc4` inline asm helpers; wrappers for all syscalls above
- `src/stdio.c`: `printf`, `puts`, `putchar` (write to stdout via `sys_write`). Supports `%s`, `%d`, `%u`, `%x`, `%ld`, `%lu`, `%lx`, `%c`, `%%`, and optional decimal width specifier (e.g. `%8lu`) for right-aligned space-padded output.
- `src/stdlib.c`: `malloc`/`free`/`calloc`/`realloc` — free-list allocator, 256 KiB arena, `[total_size][used]` header blocks
- `src/string.c`: `strlen`, `strcmp`, `strcpy`, `strncpy`, `strcat`, `strncat`, `memset`, `memcpy`
- `include/unistd.h`: O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/O_APPEND macros + syscall declarations
- `include/sys/socket.h`: `sockaddr_in`, `AF_INET`, `SOCK_STREAM`, `htons`/`ntohs`/`htonl`/`ntohl` inlines
- `include/sys/stat.h`: `struct stat` (st_mode, st_nlink, st_uid, st_gid, st_size, st_mtime), `S_ISREG`/`S_ISDIR` macros

User programs in `user/bin/`: `cat.c`, `chmod.c`, `chown.c`, `cp.c`, `df.c`, `find.c`, `free.c`, `grep.c`, `head.c`, `id.c`, `kill.c`, `login.c`, `ls.c`, `mkdir.c`, `mv.c`, `passwd.c`, `poweroff.c`, `ps.c`, `pwd.c`, `reboot.c`, `rm.c`, `sleep.c`, `sort.c`, `stat.c`, `sudo.c`, `tail.c`, `touch.c`, `uname.c`, `useradd.c`, `userdel.c`, `wc.c`, `wget.c`, `whoami.c`.

`wget.c`: parses `http://host/path`, calls `dns_resolve`, opens TCP socket, sends HTTP/1.0 GET, strips headers, writes body to stdout.

`login.c`: reads username + password, parses `/etc/passwd` (format: `name:password:uid:gid:home`), validates password, calls `spawn_as("/bin/shell", uid, gid)` so the shell starts as that user, then `waitpid`. Loops for next login.

`passwd.c`: changes a user's password entry in `/etc/passwd`; root can change any user, non-root can only change their own.

`sudo.c`: runs a command as root; checks `/etc/sudoers` or uid==0.

`useradd.c` / `userdel.c`: add/remove entries in `/etc/passwd`; root-only. `userdel` cannot delete root.

`whoami.c` / `id.c`: call `getuid()`/`getgid()`, look up name in `/etc/passwd`.

`df.c`: calls `sys_diskinfo`, displays total/used/free filesystem space.

`free.c`: calls `sys_meminfo`, displays total/used/free RAM.

`ps.c`: calls `sys_ps`, lists running processes with pid/name/uid.

`stat.c`: prints mode, size, nlink, uid, gid for each argument using `sys_stat`.

`chown.c`: `chown uid[:gid] file...`; uses `sys_chown`.

`chmod.c`: `chmod mode file...` (octal); uses `sys_chmod`.

### Multi-User Credentials (`kernel/security/cred.c`)

Each process has a `cred_t { uid, gid, euid, egid }`. All processes start as uid=0 (root). `sys_spawn` copies the parent's `cred` into the child. `sys_spawn_as` (nr=501, root-only) creates a child with a specified uid/gid — used by login to start the shell as the correct user without dropping login's own root credentials. `sys_setuid`/`sys_setgid` (nr=105/106) allow permanent privilege drops.

`/etc/passwd` format: `name:password:uid:gid:home`. Passwords are stored in plaintext. `login` reads a password and compares it against this field.

### Desktop GUI (`kernel/gui/`)

Framebuffer desktop on GOP linear framebuffer. `desktop.c`: compositor, clock, floating windows, live serial log window (drains `serial_log_read()` each tick). `fb_draw.c`: pixel/rect/text primitives. `font8x16.c`: 8×16 bitmap font. `window.c`: window management.

Serial ring buffer (`kernel/core/serial.c`): 4096-byte circular buffer. `serial_write_char` feeds it; `serial_log_read(buf, size)` drains it for the desktop log window.

### Test Harness

`kernel/test/ktest.c` provides `ktest_suite`, `ktest_check`, and the `KTEST_ASSERT` macro. Results go to serial. `kernel/test/tests.c:ktest_run_all()` calls all suites and panics if any assertion failed. Add new suites by creating `test_*.c`, declaring the run function in `tests.h`, calling it from `tests.c`, and adding the `.c` file to `KERNEL_C_SRCS` in `Makefile`.

## Known Limitations

- **ext2**: No triply-indirect block support. Max file size ~64 MiB (doubly-indirect + direct, 1024-byte blocks).
- **NVMe**: basic NVMe driver exists (`drivers/nvme.c`) but is lightly tested. AHCI (SATA) is the primary storage path.
- **Networking**: e1000 NIC only. Most real hardware has different NICs (no driver).
- **TCP**: client-only, no server accept. No TLS.
- **Processes**: no `fork`/`exec`. `sys_spawn` / `sys_spawn_as` are custom syscalls (not Linux-compatible).
- **Shell**: pipes (up to 4 stages), background jobs (`&`), Ctrl+C supported. No signal handling or job control beyond that.
- **Security**: passwords stored in plaintext in `/etc/passwd`. No PAM, no shadow file.

## Code Conventions

- C11, `-ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -mgeneral-regs-only`.
- No libc. String utilities are in `kernel/lib/string.c`. Serial I/O via `kernel/core/serial.c`.
- Shared include path: `include/` (bootloader+kernel shared headers). Kernel-only headers: `kernel/` (use `-Ikernel`).
- Panic unconditionally halts: `kernel/core/panic.c` prints to serial then executes `hlt` in a loop.
- Assembly files use NASM syntax (`-f elf64`). The `.note.GNU-stack` section must be present to suppress linker warnings.
- Never add `Co-Authored-By` lines to git commits.
