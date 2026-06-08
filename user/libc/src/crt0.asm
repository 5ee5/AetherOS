; crt0 — C runtime startup for AetherOS user programs
; Kernel writes [argc, argv[0], argv[1], ..., argv[n-1], NULL] at the initial RSP.

BITS 64
DEFAULT REL

extern main
global _start

_start:
    xor     rbp, rbp        ; clear frame pointer per ABI
    pop     rdi             ; rdi = argc (RSP now 8 bytes off 16-byte boundary)
    mov     rsi, rsp        ; rsi = argv (RSP now -> argv[0])
    and     rsp, -16        ; align to 16 bytes; call will subtract 8 → RSP+8 = 16n at main entry
    call    main
    mov     rdi, rax        ; exit status
    mov     rax, 60         ; SYS_EXIT
    syscall
.halt:
    hlt
    jmp     .halt

section .note.GNU-stack noalloc noexec nowrite progbits
