; crt0 — C runtime startup for AetherOS user programs
; Kernel writes [argc, argv[0], argv[1], ..., argv[n-1], NULL] at the initial RSP.

BITS 64
DEFAULT REL

extern main
global _start

_start:
    xor     rbp, rbp        ; clear frame pointer per ABI
    pop     rdi             ; rdi = argc
    mov     rsi, rsp        ; rsi = argv (RSP now -> argv[0])
    and     rsp, -16        ; 16-byte align
    sub     rsp, 8          ; ABI: RSP must be 16n+8 before call
    call    main
    mov     rdi, rax        ; exit status
    mov     rax, 60         ; SYS_EXIT
    syscall
.halt:
    hlt
    jmp     .halt

section .note.GNU-stack noalloc noexec nowrite progbits
