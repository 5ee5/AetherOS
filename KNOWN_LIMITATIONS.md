# Known Limitations

- The system is not yet a usable OS.
- QEMU/OVMF boot is verified, but real UEFI hardware boot has not been tested.
- The early direct map covers only the first 64 GiB of physical memory.
- The PMM tracks only the first 64 GiB of physical memory.
- Identity mappings are retained after kernel entry for bring-up simplicity.
- Kernel signature verification and secure boot compatibility are not
  implemented.
- APIC, SMP, scheduler, userspace, storage, networking, USB, audio, package
  management, and graphical desktop subsystems are not implemented.
