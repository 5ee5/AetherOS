#!/bin/sh
# Drive a real login session over the serial console and check that userland
# actually works.
#
# The boot smoke test only ever proved the kernel initializes and its in-kernel
# assertions pass.  It never logged in and never ran a single /bin program, so
# userland regressions were invisible to CI -- /bin/find, for one, was broken for
# the entire life of the project while CI stayed green.
set -eu

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
	echo "qemu-system-x86_64 is required for the userland test" >&2
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
		if [ -f "$path" ]; then printf '%s\n' "$path"; return 0; fi
	done
	return 1
}

OVMF_CODE="${OVMF_CODE:-}"
[ -n "$OVMF_CODE" ] || OVMF_CODE="$(find_ovmf || true)"
if [ -z "$OVMF_CODE" ]; then
	echo "OVMF firmware not found; set OVMF_CODE=/path/to/OVMF_CODE.fd" >&2
	exit 1
fi

for f in build/os.iso build/disk.img; do
	if [ ! -f "$f" ]; then
		echo "$f not found; run 'make iso disk' first" >&2
		exit 1
	fi
done

LOG="${USERLAND_TEST_LOG:-build/qemu-userland.log}"
mkdir -p "$(dirname "$LOG")"
: >"$LOG"

ROOT_PW="${ROOT_PASSWORD:-root}"
FIFO="$(mktemp -u "${TMPDIR:-/tmp}/aether-in.XXXXXX")"
mkfifo "$FIFO"

QPID=""
cleanup() {
	[ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
	rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

qemu-system-x86_64 \
	-machine q35 -cpu max -m 512M -smp 2 \
	-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
	-cdrom build/os.iso \
	-device ich9-ahci,id=ahci \
	-drive id=hd0,if=none,format=raw,file=build/disk.img \
	-device ide-hd,bus=ahci.0,drive=hd0 \
	-netdev user,id=n0 -device e1000,netdev=n0 \
	-serial stdio -display none -no-reboot -no-shutdown \
	<"$FIFO" >"$LOG" 2>&1 &
QPID=$!

# Hold the FIFO open for the whole session; closing it would give the guest EOF.
exec 3>"$FIFO"

fail() {
	echo "--- serial log ---" >&2
	cat "$LOG" >&2
	echo "userland test failed: $1" >&2
	exit 1
}

# Wait for a pattern to appear in the log rather than sleeping a fixed amount:
# boot time varies with host load, and a fixed sleep either flakes or wastes time.
wait_for() {
	pattern="$1"; limit="${2:-60}"; waited=0
	while [ "$waited" -lt "$limit" ]; do
		if grep -q "$pattern" "$LOG" 2>/dev/null; then return 0; fi
		if ! kill -0 "$QPID" 2>/dev/null; then fail "qemu exited early"; fi
		sleep 1
		waited=$((waited + 1))
	done
	fail "timed out after ${limit}s waiting for: $pattern"
}

send() { printf '%s\n' "$1" >&3; sleep "${2:-2}"; }

wait_for "AetherOS login:" 90
send "root" 2
wait_for "Password:" 20
send "$ROOT_PW" 3
wait_for "AetherOS shell" 30

# Each step prints a unique marker so a silent failure cannot masquerade as
# success -- several of these commands print nothing at all when they work.
send "whoami"
send "id"
send "/bin/ls -l /etc"           3
send "/bin/find /etc"            3
send "mkdir /ktest_ud"
send "/bin/rm /ktest_ud"
send "touch /ktest_ud/a.txt"
send "/bin/rmdir /ktest_ud"
send "/bin/rm /ktest_ud/a.txt"
send "/bin/rmdir /ktest_ud"
send "echo hi > /ktest_ud.txt"
send "/bin/cat /ktest_ud.txt"
send "/bin/ls /bin | /bin/grep rmdir" 3
send "/bin/rm /ktest_ud.txt"
send "df"                        3
send "free"                      3
send "echo UD_MARKER_DONE"       2

wait_for "UD_MARKER_DONE" 60

# ---- assertions ----------------------------------------------------------
# Strip CRs once: this is serial output.
PLAIN="$(mktemp "${TMPDIR:-/tmp}/aether-plain.XXXXXX")"
tr -d '\r' <"$LOG" >"$PLAIN"

need() {
	if ! grep -q "$1" "$PLAIN"; then fail "expected in output but missing: $1 ($2)"; fi
}
refute() {
	if grep -q "$1" "$PLAIN"; then fail "must not appear in output: $1 ($2)"; fi
}

need "^root$"                        "whoami printed the logged-in user"
need "uid=0(root)"                   "id resolved the name via /etc/passwd"
need "passwd"                        "/bin/ls -l /etc listed files"
need "/etc/shadow"                   "/bin/find walked the directory"
need "is a directory (use rmdir)"    "rm refuses a directory"
need "not an empty directory"        "rmdir refuses a populated directory"
need "^hi$"                          "echo redirection + cat round-tripped a file"
need "^rmdir$"                       "a two-stage pipeline carried data"

# rmdir on the emptied directory must have succeeded silently -- if it had
# failed, its error message would appear a second time.
if [ "$(grep -c 'not an empty directory' "$PLAIN")" -ne 1 ]; then
	fail "rmdir failed on an empty directory"
fi

refute "command not found"           "every command resolved"
refute "no such file"                "no spurious lookup failures"

if grep -inE "panic|segfault|exception vector|triple fault|unhandled" "$PLAIN" >/dev/null; then
	grep -inE "panic|segfault|exception vector|triple fault|unhandled" "$PLAIN" >&2
	fail "a fault or panic appeared during the userland session"
fi

rm -f "$PLAIN"
echo "userland test passed"
exit 0
