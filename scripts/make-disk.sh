#!/usr/bin/env bash
# Create build/disk.img: 10 MiB GPT disk with one ext2 partition.
set -euo pipefail

IMG=build/disk.img
PART_START_SECTORS=2048          # 1 MiB alignment
PART_OFFSET=$(( PART_START_SECTORS * 512 ))

mkdir -p build

# 10 MiB raw image
dd if=/dev/zero of="$IMG" bs=512 count=20480 status=none

# GPT label + single partition spanning the rest of the disk
parted -s "$IMG" mklabel gpt mkpart primary "${PART_START_SECTORS}s" 100%

# ext2, 1 KiB blocks, written into the partition offset inside the image
mkfs.ext2 -F -b 1024 -E offset="${PART_OFFSET}" "$IMG" >/dev/null 2>&1

# Populate /hello.txt
TMP=$(mktemp)
printf 'Hello from ext2!' > "$TMP"
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "write ${TMP} hello.txt" 2>/dev/null
rm -f "$TMP"

# Create /bin/ and populate it
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir bin" 2>/dev/null
if [ -f build/hello.elf ]; then
    debugfs -w "$IMG?offset=${PART_OFFSET}" -R "write build/hello.elf bin/hello" 2>/dev/null
fi
if [ -f build/shell.elf ]; then
    debugfs -w "$IMG?offset=${PART_OFFSET}" \
        -R "write build/shell.elf bin/shell" 2>/dev/null
fi
for prog in ls cat wc uname pwd mkdir rm cp wget grep touch head tail sort find login whoami id passwd useradd sudo poweroff reboot; do
    if [ -f "build/bin/${prog}.elf" ]; then
        debugfs -w "$IMG?offset=${PART_OFFSET}" \
            -R "write build/bin/${prog}.elf bin/${prog}" 2>/dev/null
    fi
done
# Set setuid bit on sudo (0104755 = regular + setuid + rwxr-xr-x)
if [ -f "build/bin/sudo.elf" ]; then
    debugfs -w "$IMG?offset=${PART_OFFSET}" \
        -R "sif bin/sudo mode 0104755" 2>/dev/null
fi

# Create /etc/passwd, /root, /home/user
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir etc" 2>/dev/null
TMP_PASSWD=$(mktemp)
printf 'root:root:0:0:/root\n' > "$TMP_PASSWD"
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "write ${TMP_PASSWD} etc/passwd" 2>/dev/null
rm -f "$TMP_PASSWD"
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir root" 2>/dev/null
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir home" 2>/dev/null

echo "disk: created $IMG with /hello.txt, /bin/*, /etc/passwd (root:root), /root, /home"
