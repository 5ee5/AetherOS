bits 64

section .text

extern tlb_shootdown_handler

; TLB shootdown IPI. Saves only the caller-clobbered registers, like the other
; simple IRQ stubs: tlb_shootdown_handler is ordinary C and preserves the rest.
; The handler sends the EOI itself.
global tlb_shootdown_isr
tlb_shootdown_isr:
    push    rax
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    r8
    push    r9
    push    r10
    push    r11
    call    tlb_shootdown_handler
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
