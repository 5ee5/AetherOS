#!/bin/sh
set -eu

for tool in dd mformat mmd mcopy xorriso; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "$tool is required to build the bootable ISO" >&2
		exit 1
	fi
done

rm -rf build/iso-root
mkdir -p build/iso-root/EFI/BOOT build/iso-root/EFI/OS
rm -f build/efiboot.img build/os.iso

dd if=/dev/zero of=build/efiboot.img bs=1M count=16 status=none
mformat -i build/efiboot.img ::
mmd -i build/efiboot.img ::/EFI ::/EFI/BOOT ::/EFI/OS
mcopy -i build/efiboot.img build/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i build/efiboot.img build/kernel/kernel.elf ::/EFI/OS/KERNEL.ELF
cp build/efiboot.img build/iso-root/efiboot.img
cp build/BOOTX64.EFI build/iso-root/EFI/BOOT/BOOTX64.EFI
cp build/kernel/kernel.elf build/iso-root/EFI/OS/KERNEL.ELF

xorriso -as mkisofs \
	-R \
	-J \
	-V OS \
	-e efiboot.img \
	-no-emul-boot \
	-o build/os.iso \
	build/iso-root

printf 'created build/os.iso\n'
