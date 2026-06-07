#!/bin/sh
set -eu

missing=0

require() {
	if ! command -v "$1" >/dev/null 2>&1; then
		printf 'missing required tool: %s\n' "$1" >&2
		missing=1
	fi
}

optional() {
	if ! command -v "$1" >/dev/null 2>&1; then
		printf 'missing runtime/image tool: %s\n' "$1" >&2
	fi
}

require gcc
require ld
require nasm
require objcopy
require make

optional qemu-system-x86_64
optional xorriso
optional mformat
optional mcopy
optional mmd

if [ "$missing" -ne 0 ]; then
	exit 1
fi
