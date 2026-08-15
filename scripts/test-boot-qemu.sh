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

# Without a data disk the kernel falls back to the embedded shell blob and every
# assertion below still passes -- an ext2/AHCI regression would go green.  Demand
# the image unless the caller deliberately opts out.
if [ ! -f "build/disk.img" ] && [ "${BOOT_TEST_ALLOW_NO_DISK:-0}" != "1" ]; then
	echo "build/disk.img not found; run make disk first" \
	     "(set BOOT_TEST_ALLOW_NO_DISK=1 to test without it)" >&2
	exit 1
fi

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

# The kernel never exits: it boots, runs its self-tests and idles at a login
# prompt.  Waiting for `timeout` to fire therefore cost the FULL budget on every
# run, on every leg -- so raising a timeout to cover a slow path made CI slower
# even when everything passed.  Instead, run QEMU in the background, watch the
# log for the markers that mean the boot got where it needed to, and stop it
# once they appear.  `timeout` stays as the backstop for a boot that hangs.
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
	-no-shutdown >"$LOG" 2>&1 &
QEMU_WAIT_PID=$!

# Everything the assertions below need to have been printed.
done_when() {
	grep -q "kernel: initialization complete" "$LOG" 2>/dev/null || return 1
	grep -qE "^ktest: [0-9]+ run, [0-9]+ failed" "$LOG" 2>/dev/null || return 1
	if [ "${BOOT_TEST_USB:-0}" = "1" ]; then
		grep -q "vfs: mounted ext2 root on usb" "$LOG" 2>/dev/null || return 1
		grep -q "usb-hid: keyboard ready" "$LOG" 2>/dev/null || return 1
	fi
	return 0
}

while kill -0 "$QEMU_WAIT_PID" 2>/dev/null; do
	if done_when; then
		# Keep running briefly: a fault on the post-init path (the login/shell
		# handoff) is exactly what the old assertions could not see, and killing
		# the instant the markers land would reintroduce that blind spot.
		sleep "${BOOT_TEST_GRACE:-5}"
		kill "$QEMU_WAIT_PID" 2>/dev/null
		break
	fi
	sleep 1
done

wait "$QEMU_WAIT_PID"
status=$?
# 143 = SIGTERM from the early stop above, and 124 = the timeout backstop; both
# are normal endings for a kernel that never exits on its own.
if [ "$status" -eq 143 ]; then
	status=0
fi
set -e

fail() {
	cat "$LOG"
	echo "boot smoke test failed: $1" >&2
	exit 1
}

# The kernel idles forever under -no-reboot -no-shutdown, so `timeout` killing
# QEMU (124) is the normal, successful outcome.  Any other non-zero status means
# QEMU itself failed to start or died -- previously this was captured and never
# looked at, so a QEMU crash and a clean run were indistinguishable.
if [ "$status" -ne 124 ] && [ "$status" -ne 0 ]; then
	fail "qemu exited with status $status"
fi

if ! grep -q "kernel: initialization complete" "$LOG"; then
	fail "kernel did not reach initialization complete"
fi

# Nothing may fault, at any point -- including AFTER init completes, on the
# login/shell path.  The kernel prints none of these on a healthy boot, so any
# occurrence is a real failure.  Audit finding H23 (2026-08-11) was exactly such
# a post-init user fault, and the old assertions could not see it.
if grep -inE "panic|segfault|exception vector|triple fault|unhandled" "$LOG" >/dev/null; then
	grep -inE "panic|segfault|exception vector|triple fault|unhandled" "$LOG" >&2
	fail "a fault or panic appeared in the boot log"
fi

if grep -q "ktest: FAIL" "$LOG"; then
	fail "kernel self-tests reported failures"
fi

# Assert the summary line and the actual count, not merely that "ktest:" appeared
# somewhere.  A run that silently executed 3 assertions instead of ~8500 used to
# pass this test just as readily as a full one.
# tr -d '\r': this is serial output, so every line carries a trailing CR that
# would defeat a '$'-anchored match.
KTEST_LINE="$(tr -d '\r' <"$LOG" | grep -E "^ktest: [0-9]+ run, [0-9]+ failed$" | tail -1)"
if [ -z "$KTEST_LINE" ]; then
	fail "no ktest summary line found"
fi

KTEST_RUN="$(printf '%s' "$KTEST_LINE" | sed -E 's/^ktest: ([0-9]+) run.*/\1/')"
KTEST_FAILED="$(printf '%s' "$KTEST_LINE" | sed -E 's/^.*, ([0-9]+) failed$/\1/')"
MIN_KTESTS="${BOOT_TEST_MIN_KTESTS:-8000}"

# Guard the extraction itself: a [ comparison against a non-integer is not fatal
# inside an `if`, even under set -e, so a silently failed parse would otherwise
# read as "no failures".
case "$KTEST_RUN$KTEST_FAILED" in
	''|*[!0-9]*) fail "could not parse ktest summary: '$KTEST_LINE'" ;;
esac

if [ "$KTEST_FAILED" -ne 0 ]; then
	fail "$KTEST_FAILED ktest assertions failed"
fi

if [ "$KTEST_RUN" -lt "$MIN_KTESTS" ]; then
	fail "only $KTEST_RUN ktest assertions ran (expected at least $MIN_KTESTS)"
fi

# In USB mode the data disk is attached ONLY over USB, so the root must have
# been mounted through the xHCI/BOT/SCSI stack — otherwise it silently fell
# back to the embedded shell and the test would be meaningless.
if [ "${BOOT_TEST_USB:-0}" = "1" ] && ! grep -q "vfs: mounted ext2 root on usb" "$LOG"; then
	fail "USB root was not mounted via the USB stack"
fi

# USB mode also attaches a USB HID keyboard: assert it enumerated alongside the
# storage device (proves multi-device xHCI + the HID driver).
if [ "${BOOT_TEST_USB:-0}" = "1" ] && ! grep -q "usb-hid: keyboard ready" "$LOG"; then
	fail "USB HID keyboard did not enumerate"
fi

echo "boot smoke test passed ($KTEST_RUN ktest assertions, 0 failed)"
exit 0
