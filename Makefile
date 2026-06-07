BUILD_DIR := build
BOOT_BUILD := $(BUILD_DIR)/boot
KERNEL_BUILD := $(BUILD_DIR)/kernel
ESP_DIR := $(BUILD_DIR)/esp

CC ?= gcc
LD ?= ld
NASM ?= nasm
OBJCOPY ?= objcopy

COMMON_WARN := -Wall -Wextra -Werror -Wno-unused-parameter
COMMON_CFLAGS := -std=c11 -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-stack-check -fno-lto -m64 -mno-red-zone -mgeneral-regs-only \
	$(COMMON_WARN) -Iinclude

KERNEL_CFLAGS := $(COMMON_CFLAGS) -fno-pic -fno-pie -mcmodel=kernel -Ikernel
BOOT_CFLAGS := $(COMMON_CFLAGS) -fpic -fshort-wchar -maccumulate-outgoing-args

KERNEL_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld
BOOT_LDFLAGS := -nostdlib -shared -Bsymbolic -T boot/uefi/linker.ld

KERNEL_C_SRCS := \
	kernel/kernel.c \
	kernel/acpi/acpi.c \
	kernel/arch/x86_64/apic.c \
	kernel/arch/x86_64/cpu.c \
	kernel/arch/x86_64/gdt.c \
	kernel/arch/x86_64/idt.c \
	kernel/arch/x86_64/ioapic.c \
	kernel/arch/x86_64/smp.c \
	kernel/arch/x86_64/tss.c \
	kernel/block/gpt.c \
	kernel/core/fb.c \
	kernel/core/panic.c \
	kernel/core/serial.c \
	kernel/drivers/ahci.c \
	kernel/drivers/e1000.c \
	kernel/drivers/pci.c \
	kernel/drivers/ps2kbd.c \
	kernel/exec/elf.c \
	kernel/fs/ext2.c \
	kernel/fs/vfs.c \
	kernel/gui/desktop.c \
	kernel/gui/fb_draw.c \
	kernel/gui/font8x16.c \
	kernel/gui/window.c \
	kernel/lib/string.c \
	kernel/mem/heap.c \
	kernel/mem/pmm.c \
	kernel/mem/vmm.c \
	kernel/net/arp.c \
	kernel/net/checksum.c \
	kernel/net/dhcp.c \
	kernel/net/dns.c \
	kernel/net/eth.c \
	kernel/net/icmp.c \
	kernel/net/ipv4.c \
	kernel/net/net.c \
	kernel/net/nic.c \
	kernel/net/tcp.c \
	kernel/net/udp.c \
	kernel/proc/fd.c \
	kernel/proc/process.c \
	kernel/sched/sched.c \
	kernel/sched/thread.c \
	kernel/security/cred.c \
	kernel/sync/mutex.c \
	kernel/sync/spinlock.c \
	kernel/syscall/syscall.c \
	kernel/test/ktest.c \
	kernel/test/test_pmm.c \
	kernel/test/test_vmm.c \
	kernel/test/test_string.c \
	kernel/test/test_heap.c \
	kernel/test/test_sched.c \
	kernel/test/test_userspace.c \
	kernel/test/test_vfs.c \
	kernel/test/tests.c

KERNEL_ASM_SRCS := \
	kernel/arch/x86_64/entry.asm \
	kernel/arch/x86_64/gdt_load.asm \
	kernel/arch/x86_64/interrupts.asm \
	kernel/arch/x86_64/sched_asm.asm \
	kernel/arch/x86_64/syscall_entry.asm \
	kernel/arch/x86_64/smp_trampoline_blob.asm \
	kernel/drivers/ps2kbd_isr.asm \
	user/hello/hello_blob.asm \
	user/shell/shell_blob.asm

BOOT_C_SRCS := boot/uefi/main.c

TRAMPOLINE_BIN := $(BUILD_DIR)/smp_trampoline.bin
HELLO_ELF := $(BUILD_DIR)/hello.elf
SHELL_ELF := $(BUILD_DIR)/shell.elf
DISK_IMG := $(BUILD_DIR)/disk.img

KERNEL_OBJS := $(KERNEL_C_SRCS:%.c=$(BUILD_DIR)/%.o) \
	$(KERNEL_ASM_SRCS:%.asm=$(BUILD_DIR)/%.o)
BOOT_OBJS := $(BOOT_C_SRCS:%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean esp iso disk check-deps run-qemu test boot-test

all: esp

check-deps:
	./scripts/check-deps.sh

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(if $(filter boot/%,$<),$(BOOT_CFLAGS),$(KERNEL_CFLAGS)) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

# Flat-binary AP startup trampoline
$(TRAMPOLINE_BIN): kernel/arch/x86_64/smp_trampoline.asm
	@mkdir -p $(@D)
	$(NASM) -f bin -o $@ $<

# The blob object depends on the binary being present
$(BUILD_DIR)/kernel/arch/x86_64/smp_trampoline_blob.o: \
	kernel/arch/x86_64/smp_trampoline_blob.asm $(TRAMPOLINE_BIN)
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

# User-mode hello binary: assemble + link at 0x400000
$(BUILD_DIR)/hello.o: user/hello/hello.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 -o $@ $<

$(HELLO_ELF): $(BUILD_DIR)/hello.o
	$(LD) -m elf_x86_64 -Ttext=0x400000 -e _start --build-id=none -o $@ $<

# The blob object depends on the ELF binary
$(BUILD_DIR)/user/hello/hello_blob.o: \
	user/hello/hello_blob.asm $(HELLO_ELF)
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

# Shell binary: assemble + link at 0x400000
$(BUILD_DIR)/shell.o: user/shell/shell.asm
	@mkdir -p $(@D)
	$(NASM) -f elf64 -o $@ $<

$(SHELL_ELF): $(BUILD_DIR)/shell.o
	$(LD) -m elf_x86_64 -Ttext=0x400000 -e _start --build-id=none -o $@ $<

$(BUILD_DIR)/user/shell/shell_blob.o: \
	user/shell/shell_blob.asm $(SHELL_ELF)
	@mkdir -p $(@D)
	$(NASM) -f elf64 $< -o $@

$(KERNEL_BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(@D)
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

$(BOOT_BUILD)/bootloader.so: $(BOOT_OBJS) boot/uefi/linker.ld
	@mkdir -p $(@D)
	$(LD) $(BOOT_LDFLAGS) -o $@ $(BOOT_OBJS)

$(BUILD_DIR)/BOOTX64.EFI: $(BOOT_BUILD)/bootloader.so
	$(OBJCOPY) -j .text -j .reloc -O pei-x86-64 \
		--subsystem=efi-app $< $@

esp: $(BUILD_DIR)/BOOTX64.EFI $(KERNEL_BUILD)/kernel.elf
	@mkdir -p $(ESP_DIR)/EFI/BOOT $(ESP_DIR)/EFI/OS
	cp $(BUILD_DIR)/BOOTX64.EFI $(ESP_DIR)/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL_BUILD)/kernel.elf $(ESP_DIR)/EFI/OS/KERNEL.ELF

$(DISK_IMG):
	./scripts/make-disk.sh

disk: $(DISK_IMG)

run-qemu: esp disk
	./scripts/run-qemu.sh

iso: esp
	./scripts/mkiso.sh

boot-test: iso disk
	./scripts/test-boot-qemu.sh

test: boot-test

clean:
	rm -rf $(BUILD_DIR)

-include $(KERNEL_OBJS:.o=.d)
-include $(BOOT_OBJS:.o=.d)
