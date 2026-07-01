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

DISK_ARGS=""
if [ -f "build/disk.img" ]; then
	if [ "${BOOT_TEST_USB:-0}" = "1" ]; then
		# Attach the data disk ONLY as a USB mass-storage device behind an
		# emulated xHCI controller (forces the root mount through the USB stack),
		# plus a USB HID keyboard so the multi-device path + HID driver run.
		DISK_ARGS="-device qemu-xhci,id=xhci -drive id=usb,if=none,format=raw,file=build/disk.img -device usb-storage,bus=xhci.0,drive=usb -device usb-kbd,bus=xhci.0"
	else
		DISK_ARGS="-device ich9-ahci,id=ahci -drive id=hd0,if=none,format=raw,file=build/disk.img -device ide-hd,bus=ahci.0,drive=hd0"
	fi
fi

set +e
# shellcheck disable=SC2086
timeout "${BOOT_TEST_TIMEOUT:-60s}" qemu-system-x86_64 \
	-machine q35 \
	-cpu max \
	-m 512M \
	-smp 2 \
	-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
	-cdrom build/os.iso \
	$DISK_ARGS \
	-netdev user,id=n0 -device e1000,netdev=n0 \
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

# In USB mode the data disk is attached ONLY over USB, so the root must have
# been mounted through the xHCI/BOT/SCSI stack — otherwise it silently fell
# back to the embedded shell and the test would be meaningless.
if [ "${BOOT_TEST_USB:-0}" = "1" ] && ! grep -q "vfs: mounted ext2 root on usb" "$LOG"; then
	cat "$LOG"
	echo "boot smoke test failed: USB root was not mounted via the USB stack" >&2
	exit 1
fi

# USB mode also attaches a USB HID keyboard: assert it enumerated alongside the
# storage device (proves multi-device xHCI + the HID driver).
if [ "${BOOT_TEST_USB:-0}" = "1" ] && ! grep -q "usb-hid: keyboard ready" "$LOG"; then
	cat "$LOG"
	echo "boot smoke test failed: USB HID keyboard did not enumerate" >&2
	exit 1
fi

echo "boot smoke test passed"
exit 0
