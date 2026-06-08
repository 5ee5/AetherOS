#!/bin/sh
set -eu

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
	echo "qemu-system-x86_64 is required to run the OS" >&2
	exit 1
fi

find_ovmf() {
	for path in \
		/usr/share/edk2/OvmfX64/OVMF_CODE.fd \
		/usr/share/OVMF/OVMF_CODE.fd \
		/usr/share/ovmf/OVMF.fd \
		/usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
		/usr/share/qemu/OVMF.fd
	do
		if [ -f "$path" ]; then
			printf '%s\n' "$path"
			return 0
		fi
	done
	return 1
}

OVMF_CODE="${OVMF_CODE:-}"
if [ -z "$OVMF_CODE" ]; then
	OVMF_CODE="$(find_ovmf || true)"
fi

if [ -z "$OVMF_CODE" ]; then
	echo "OVMF firmware not found; set OVMF_CODE=/path/to/OVMF_CODE.fd" >&2
	exit 1
fi

DISK_ARGS=""
if [ -f "build/disk.img" ]; then
	DISK_ARGS="-device ich9-ahci,id=ahci -drive id=hd0,if=none,format=raw,file=build/disk.img -device ide-hd,bus=ahci.0,drive=hd0"
fi

exec qemu-system-x86_64 \
	-machine q35 \
	-cpu max \
	-m 512M \
	-smp 2 \
	-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
	-drive format=raw,file=fat:rw:build/esp \
	$DISK_ARGS \
	-netdev user,id=n0 -device e1000,netdev=n0 \
	-serial stdio \
	-display none \
	-no-reboot \
	-no-shutdown
