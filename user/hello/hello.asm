; First user-mode process.
; Exercises sys_write, sys_open, sys_read, sys_close, sys_getpid, sys_exit.
; Linux x86-64 syscall ABI: nr in rax, args in rdi rsi rdx r10 r8 r9.

bits 64

global _start

section .text
_start:
    ; sys_write(1, hello_msg, hello_len)
    mov     rax, 1
    mov     rdi, 1
    lea     rsi, [rel hello_msg]
    mov     rdx, hello_len
    syscall

    ; sys_open("/hello.txt", O_RDONLY=0, 0)
    mov     rax, 2
    lea     rdi, [rel filename]
    xor     rsi, rsi
    xor     rdx, rdx
    syscall
    ; save fd in rbx
    mov     rbx, rax

    ; if fd < 0, skip read
    test    rax, rax
    js      .no_file

    ; sys_read(fd, read_buf, 64)
    mov     rax, 0
    mov     rdi, rbx
    lea     rsi, [rel read_buf]
    mov     rdx, 64
    syscall
    ; save bytes read in r12
    mov     r12, rax

    ; sys_write(1, read_buf, bytes_read)
    test    r12, r12
    jle     .skip_write
    mov     rax, 1
    mov     rdi, 1
    lea     rsi, [rel read_buf]
    mov     rdx, r12
    syscall
.skip_write:

    ; sys_close(fd)
    mov     rax, 3
    mov     rdi, rbx
    syscall

.no_file:
    ; sys_getpid() — result ignored
    mov     rax, 39
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
hello_msg:  db "Hello from userspace!", 10
hello_len   equ $ - hello_msg
filename:   db "/hello.txt", 0

section .bss
read_buf:   resb 64
