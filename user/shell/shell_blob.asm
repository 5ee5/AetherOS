; Embeds the shell ELF binary as a read-only kernel data blob.

section .rodata
global shell_elf_start
global shell_elf_end

shell_elf_start:
    incbin "build/shell.elf"
shell_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
