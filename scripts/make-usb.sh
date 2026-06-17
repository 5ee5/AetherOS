#!/usr/bin/env bash
# Create build/usb.img: 64 MiB bootable USB image.
#
# Layout (ESP FIRST for real-firmware compatibility):
#   GPT idx 0 (LBA 2048–67583):   FAT16 ESP            — UEFI firmware boots \EFI\BOOT\BOOTX64.EFI
#   GPT idx 1 (LBA 67584–end):    ext2 data partition  — kernel mounts the ext2 partition by content
#
# Rationale: many real UEFI firmwares only honour an ESP that is the first GPT
# partition (or scan removable media quirkily), unlike OVMF which finds it by
# type GUID anywhere. The ESP is FAT16 — UEFI permits FAT12/16/32 on removable
# media, and FAT16 is reliable at this ~32 MiB size where forced FAT32 is
# borderline/non-conformant and gets rejected by strict firmware FAT drivers.
set -euo pipefail

IMG=build/usb.img
ESP_IMG=build/esp_fat.img

ESP_START=2048
ESP_END=67583
EXT2_START=67584
TOTAL_SECTORS=131072   # 64 MiB

EXT2_OFFSET=$(( EXT2_START * 512 ))
ESP_OFFSET=$(( ESP_START * 512 ))
ESP_SECTORS=$(( ESP_END - ESP_START + 1 ))

# GPT reserves the last 33 sectors for the backup partition table (+1 for the
# backup header). Size the ext2 filesystem to stay clear of it, otherwise
# mkfs.ext2 (no explicit size) extends to end-of-file and corrupts the backup GPT.
EXT2_LAST=$(( TOTAL_SECTORS - 34 ))               # last LBA usable by partition 2
EXT2_BLOCKS=$(( (EXT2_LAST - EXT2_START + 1) * 512 / 1024 ))

mkdir -p build

# 64 MiB raw image
dd if=/dev/zero of="$IMG" bs=512 count=$TOTAL_SECTORS status=none

# GPT: FAT ESP first (firmware boots it), ext2 data second
parted -s "$IMG" mklabel gpt \
    mkpart ESP   fat16 "${ESP_START}s"  "${ESP_END}s" \
    mkpart data  ext2  "${EXT2_START}s" 100% \
    set 1 esp on

# Write ext2 filesystem into partition 1
mkfs.ext2 -F -b 1024 -E offset="${EXT2_OFFSET}" "$IMG" "${EXT2_BLOCKS}" >/dev/null 2>&1

# Populate ext2: /hello.txt and /bin/*
TMP=$(mktemp)
printf 'Hello from ext2!' > "$TMP"
debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "write ${TMP} hello.txt" 2>/dev/null
rm -f "$TMP"

debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "mkdir bin" 2>/dev/null
if [ -f build/hello.elf ]; then
    debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "write build/hello.elf bin/hello" 2>/dev/null
fi
if [ -f build/shell.elf ]; then
    debugfs -w "$IMG?offset=${EXT2_OFFSET}" \
        -R "write build/shell.elf bin/shell" 2>/dev/null
fi
for elf in build/bin/*.elf; do
    [ -f "$elf" ] || continue
    prog=$(basename "$elf" .elf)
    debugfs -w "$IMG?offset=${EXT2_OFFSET}" \
        -R "write $elf bin/${prog}" 2>/dev/null
done
# Set setuid bit on sudo (0104755 = regular + setuid + rwxr-xr-x)
if [ -f "build/bin/sudo.elf" ]; then
    debugfs -w "$IMG?offset=${EXT2_OFFSET}" \
        -R "sif bin/sudo mode 0104755" 2>/dev/null
fi

# Create /etc/passwd, /root, /home/user
debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "mkdir etc" 2>/dev/null
TMP_PASSWD=$(mktemp)
printf 'root:root:0:0:/root\n' > "$TMP_PASSWD"
debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "write ${TMP_PASSWD} etc/passwd" 2>/dev/null
rm -f "$TMP_PASSWD"
debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "mkdir root" 2>/dev/null
debugfs -w "$IMG?offset=${EXT2_OFFSET}" -R "mkdir home" 2>/dev/null

# Build temporary FAT16 ESP image (deterministic FAT16 via dosfstools)
dd if=/dev/zero of="$ESP_IMG" bs=512 count=$ESP_SECTORS status=none
mkfs.fat -F 16 -n ESP "$ESP_IMG" >/dev/null
mmd -i "$ESP_IMG" ::/EFI ::/EFI/BOOT ::/EFI/OS
mcopy -i "$ESP_IMG" build/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" build/kernel/kernel.elf ::/EFI/OS/KERNEL.ELF

# Splice FAT32 image into the USB image at the ESP partition offset
dd if="$ESP_IMG" of="$IMG" bs=512 seek=$ESP_START conv=notrunc status=none
rm -f "$ESP_IMG"

echo "usb: created $IMG (64 MiB, FAT16 ESP + ext2)"
echo "     flash: dd if=$IMG of=/dev/sdX bs=4M status=progress oflag=sync"
