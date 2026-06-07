# Changelog

## Unreleased

- Added initial build system, scripts, and documentation.
- Added custom UEFI bootloader source.
- Added higher-half kernel skeleton with serial logging, GDT, IDT, PMM, VMM, and
  panic handling.
- Added ESP staging and gated ISO generation script.
- Fixed EFI PE/COFF section layout so OVMF starts `BOOTX64.EFI`.
- Changed the El Torito boot image to a firmware-compatible FAT format.
- Added QEMU boot smoke test and wired it to `make test`.
- Verified QEMU/OVMF boot from `build/os.iso` through kernel initialization.
