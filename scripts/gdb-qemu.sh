#!/bin/sh
set -eu

if ! command -v gdb >/dev/null 2>&1; then
	echo "gdb is required" >&2
	exit 1
fi

exec gdb -q -ex "target remote :1234" -ex "symbol-file build/kernel/kernel.elf"

