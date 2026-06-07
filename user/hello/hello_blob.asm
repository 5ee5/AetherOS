; Embeds the hello ELF binary as a read-only kernel data blob.

section .rodata
global hello_elf_start
global hello_elf_end

hello_elf_start:
    incbin "build/hello.elf"
hello_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
