; Embeds the flat-binary trampoline into the kernel's .rodata section.
; The binary is assembled separately (make rule produces build/smp_trampoline.bin).

section .rodata
global smp_trampoline_start
global smp_trampoline_end

smp_trampoline_start:
    incbin "build/smp_trampoline.bin"
smp_trampoline_end:

section .note.GNU-stack noalloc noexec nowrite progbits
