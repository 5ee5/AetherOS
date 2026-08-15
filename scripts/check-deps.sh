#!/bin/sh
set -eu

missing=0

# Needed to build the kernel and bootloader -- `make` cannot get off the ground
# without these.
require() {
	if ! command -v "$1" >/dev/null 2>&1; then
		printf 'missing required tool: %s (%s)\n' "$1" "$2" >&2
		missing=1
	fi
}

# Needed only by specific targets.  Reported but not fatal, so a plain `make`
# still works on a machine that cannot build images.
optional() {
	if ! command -v "$1" >/dev/null 2>&1; then
		printf 'missing tool for %s: %s\n' "$2" "$1" >&2
	fi
}

# --- build ---
require gcc      "compiling kernel and userland"
require ld       "linking"
require nasm     "assembling"
require objcopy  "producing BOOTX64.EFI"
require make     "the build itself"

# --- make iso ---
optional xorriso  "make iso"
optional mformat  "make iso"
optional mcopy    "make iso"
optional mmd      "make iso"
optional mkfs.fat "make iso"

# --- make disk / make usb ---
# These were absent from this script while README listed them as requirements,
# so the documented "verify tools" step passed and the build then died with a
# raw "parted: command not found".
optional parted    "make disk / make usb"
optional mkfs.ext2 "make disk / make usb"
optional debugfs   "make disk / make usb (populates the ext2 image)"
optional dd        "make disk / make usb"

# --- make test ---
optional qemu-system-x86_64 "make test"
optional timeout            "make test"

if [ "$missing" -ne 0 ]; then
	exit 1
fi
