bits 64

section .text
extern x86_64_exception_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
	cli
	push qword 0
	push qword %1
	jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
	cli
	push qword %1
	jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR 29
ISR_ERR 30
ISR_NOERR 31

isr_common:
	cld
	push rax
	push rbx
	push rcx
	push rdx
	push rbp
	push rdi
	push rsi
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	mov rdi, rsp
	call x86_64_exception_dispatch
	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop r10
	pop r9
	pop r8
	pop rsi
	pop rdi
	pop rbp
	pop rdx
	pop rcx
	pop rbx
	pop rax
	add rsp, 16
	iretq

section .rodata
global x86_64_isr_stub_table
x86_64_isr_stub_table:
	dq isr0
	dq isr1
	dq isr2
	dq isr3
	dq isr4
	dq isr5
	dq isr6
	dq isr7
	dq isr8
	dq isr9
	dq isr10
	dq isr11
	dq isr12
	dq isr13
	dq isr14
	dq isr15
	dq isr16
	dq isr17
	dq isr18
	dq isr19
	dq isr20
	dq isr21
	dq isr22
	dq isr23
	dq isr24
	dq isr25
	dq isr26
	dq isr27
	dq isr28
	dq isr29
	dq isr30
	dq isr31

section .note.GNU-stack noalloc noexec nowrite progbits
