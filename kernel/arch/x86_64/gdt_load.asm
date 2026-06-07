bits 64

section .text
global x86_64_gdt_load

x86_64_gdt_load:
	lgdt [rdi]
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	push qword 0x08
	lea rax, [rel .reload_cs]
	push rax
	retfq
.reload_cs:
	ret

section .note.GNU-stack noalloc noexec nowrite progbits
