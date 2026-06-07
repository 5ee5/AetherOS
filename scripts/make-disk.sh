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

# Create /bin/ and add hello.elf so the shell can spawn it
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir bin" 2>/dev/null
if [ -f build/hello.elf ]; then
    debugfs -w "$IMG?offset=${PART_OFFSET}" -R "write build/hello.elf bin/hello" 2>/dev/null
fi

echo "disk: created $IMG with /hello.txt and /bin/hello"
