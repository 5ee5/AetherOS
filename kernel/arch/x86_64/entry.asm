bits 64

section .text.entry
global kernel_entry
extern kernel_main

kernel_entry:
	cli
	mov rsp, kernel_stack_top
	xor rbp, rbp
	call kernel_main

.halt:
	hlt
	jmp .halt

section .bss
align 4096
global kernel_stack_guard
kernel_stack_guard:
	resb 4096
kernel_stack:
	resb 65536
global kernel_stack_top
kernel_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
