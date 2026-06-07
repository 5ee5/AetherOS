bits 64

section .text

extern ps2kbd_handler

global ps2kbd_isr
ps2kbd_isr:
    push    rax
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    r8
    push    r9
    push    r10
    push    r11
    call    ps2kbd_handler
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rax
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
