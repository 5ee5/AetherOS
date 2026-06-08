#!/usr/bin/env bash
# Create build/usb.img: 64 MiB bootable USB image.
#
# Layout:
#   GPT idx 0 (LBA 2048–67583):   ext2 data partition  — kernel mounts parts[0]
#   GPT idx 1 (LBA 67584–end):    FAT32 ESP            — UEFI firmware finds by type GUID
#
set -euo pipefail

IMG=build/usb.img
ESP_IMG=build/esp_fat.img

EXT2_START=2048
EXT2_END=67583
ESP_START=67584
TOTAL_SECTORS=131072   # 64 MiB

EXT2_OFFSET=$(( EXT2_START * 512 ))
ESP_OFFSET=$(( ESP_START * 512 ))
ESP_SECTORS=$(( TOTAL_SECTORS - ESP_START ))

mkdir -p build

# 64 MiB raw image
dd if=/dev/zero of="$IMG" bs=512 count=$TOTAL_SECTORS status=none

# GPT: ext2 data first, FAT32 ESP second (firmware finds ESP by type GUID)
parted -s "$IMG" mklabel gpt \
    mkpart data  ext2  "${EXT2_START}s" "${EXT2_END}s" \
    mkpart ESP   fat32 "${ESP_START}s"  100% \
    set 2 esp on

# Write ext2 filesystem into partition 1
mkfs.ext2 -F -b 1024 -E offset="${EXT2_OFFSET}" "$IMG" >/dev/null 2>&1

# Populate ext2: /hello.txt and /bin/*
TMP=$(mktemp)
printf 'Hello from ext2!' > "$TMP"
debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "write ${TMP} hello.txt" 2>/dev/null
rm -f "$TMP"

debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "mkdir bin" 2>/dev/null
if [ -f build/hello.elf ]; then
    debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "write build/hello.elf bin/hello" 2>/dev/null
fi
for prog in ls cat wc uname pwd mkdir rm cp wget; do
    if [ -f "build/bin/${prog}.elf" ]; then
        debugfs -w "$IMG?offset=${EXT2_OFFSET}" \
            -R "write build/bin/${prog}.elf bin/${prog}" 2>/dev/null
    fi
done

# Build temporary FAT32 ESP image
dd if=/dev/zero of="$ESP_IMG" bs=512 count=$ESP_SECTORS status=none
mformat -i "$ESP_IMG" -F -h 255 -s 63 ::
mmd -i "$ESP_IMG" ::/EFI ::/EFI/BOOT ::/EFI/OS
mcopy -i "$ESP_IMG" build/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" build/kernel/kernel.elf ::/EFI/OS/KERNEL.ELF

# Splice FAT32 image into the USB image at the ESP partition offset
dd if="$ESP_IMG" of="$IMG" bs=512 seek=$ESP_START conv=notrunc status=none
rm -f "$ESP_IMG"

echo "usb: created $IMG (64 MiB, ext2 + FAT32 ESP)"
echo "     flash: dd if=$IMG of=/dev/sdX bs=4M status=progress oflag=sync"
