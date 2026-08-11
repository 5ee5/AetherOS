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
for elf in build/bin/*.elf; do
    [ -f "$elf" ] || continue
    prog=$(basename "$elf" .elf)
    debugfs -w "$IMG?offset=${PART_OFFSET}" \
        -R "write $elf bin/${prog}" 2>/dev/null
done
# Setuid root (0104755 = regular + setuid + rwxr-xr-x).
#   sudo   — needs root to read /etc/shadow and to spawn as another uid
#   passwd — lets an unprivileged user rewrite their own hash in /etc/shadow,
#            which is 0600; without this, `passwd` only works for root
for prog in sudo passwd; do
    if [ -f "build/bin/${prog}.elf" ]; then
        debugfs -w "$IMG?offset=${PART_OFFSET}" \
            -R "sif bin/${prog} mode 0104755" 2>/dev/null
    fi
done

# Account database.  Secrets live in /etc/shadow (0600) so that /etc/passwd can
# stay world-readable (0644) — every tool needs it to map uid -> name, and it
# used to be 0600, which is why `whoami` printed a bare number for non-root.
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir etc" 2>/dev/null

ROOT_PASSWORD="${ROOT_PASSWORD:-root}"
ROOT_HASH=$(build/mkpasswd "$ROOT_PASSWORD")

TMP=$(mktemp)
printf 'root:x:0:0:/root\n' > "$TMP"
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "write ${TMP} etc/passwd" 2>/dev/null

printf 'root:%s\n' "$ROOT_HASH" > "$TMP"
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "write ${TMP} etc/shadow" 2>/dev/null

# Fail closed: an empty sudoers means only root may use sudo.  Add one account
# name per line to grant it.
cat > "$TMP" <<'SUDOERS'
# One account name per line may run commands as root via sudo.
# root does not need to be listed.
SUDOERS
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "write ${TMP} etc/sudoers" 2>/dev/null
rm -f "$TMP"

debugfs -w "$IMG?offset=${PART_OFFSET}" -R "sif etc/passwd mode 0100644" 2>/dev/null
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "sif etc/shadow mode 0100600" 2>/dev/null
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "sif etc/sudoers mode 0100600" 2>/dev/null

debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir root" 2>/dev/null
debugfs -w "$IMG?offset=${PART_OFFSET}" -R "mkdir home" 2>/dev/null

echo "disk: created $IMG with /hello.txt, /bin/*, /etc/{passwd,shadow,sudoers}, /root, /home"
echo "disk: root password is '${ROOT_PASSWORD}' (override with ROOT_PASSWORD=...)"
