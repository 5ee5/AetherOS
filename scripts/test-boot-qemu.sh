#!/bin/sh
set -eu

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
	echo "qemu-system-x86_64 is required for the boot smoke test" >&2
	exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
	echo "timeout is required for the boot smoke test" >&2
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

if [ ! -f build/os.iso ]; then
	echo "build/os.iso not found; run make iso first" >&2
	exit 1
fi

LOG="${BOOT_TEST_LOG:-build/qemu-boot.log}"
mkdir -p "$(dirname "$LOG")"

set +e
timeout "${BOOT_TEST_TIMEOUT:-30s}" qemu-system-x86_64 \
	-machine q35 \
	-cpu max \
	-m 512M \
	-smp 2 \
	-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
	-cdrom build/os.iso \
	-serial stdio \
	-display none \
	-no-reboot \
	-no-shutdown >"$LOG" 2>&1
status=$?
set -e

if ! grep -q "kernel: initialization complete" "$LOG"; then
	cat "$LOG"
	echo "boot smoke test failed: kernel did not reach initialization complete" >&2
	exit 1
fi

if grep -q "ktest: FAIL" "$LOG"; then
	cat "$LOG"
	echo "boot smoke test failed: kernel self-tests reported failures" >&2
	exit 1
fi

if ! grep -q "ktest:" "$LOG"; then
	cat "$LOG"
	echo "boot smoke test failed: no ktest output found" >&2
	exit 1
fi

echo "boot smoke test passed"
exit 0
