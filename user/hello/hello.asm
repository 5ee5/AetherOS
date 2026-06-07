; First user-mode process.
; Calls sys_write(1, msg, len) then sys_exit(0).
; Linux x86-64 syscall ABI: nr in rax, args in rdi rsi rdx r10 r8 r9.

bits 64

global _start

section .text
_start:
    ; sys_write(1, msg, msglen)
    mov     rax, 1
    mov     rdi, 1
    lea     rsi, [rel msg]
    mov     rdx, msglen
    syscall

    ; sys_exit(0)
    mov     rax, 60
    xor     rdi, rdi
    syscall

    ; Unreachable.
.hang:
    hlt
    jmp     .hang

section .rodata
msg:    db "Hello from userspace!", 10
msglen  equ $ - msg
