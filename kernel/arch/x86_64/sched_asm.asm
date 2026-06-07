bits 64

section .text

extern sched_tick
extern thread_exit

; ---- APIC timer ISR (vector 0x20) ---------------------------------------
;
; Stack layout after all pushes (RSP points to r15 slot):
;   [rsp+  0] r15
;   [rsp+  8] r14
;   [rsp+ 16] r13
;   [rsp+ 24] r12
;   [rsp+ 32] r11
;   [rsp+ 40] r10
;   [rsp+ 48] r9
;   [rsp+ 56] r8
;   [rsp+ 64] rsi
;   [rsp+ 72] rdi
;   [rsp+ 80] rbp
;   [rsp+ 88] rdx
;   [rsp+ 96] rcx
;   [rsp+104] rbx
;   [rsp+112] rax
;   [rsp+120] RIP   (hardware)
;   [rsp+128] CS
;   [rsp+136] RFLAGS
;   [rsp+144] RSP   (interrupted stack pointer)
;   [rsp+152] SS
;
global apic_timer_isr
apic_timer_isr:
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

    mov rdi, rsp        ; pass current RSP to sched_tick
    call sched_tick     ; returns new RSP (may be a different thread)
    mov rsp, rax        ; switch to new stack

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

    iretq

; ---- thread_run_trampoline -----------------------------------------------
;
; Called as the initial RIP of a newly created thread (via iretq).
; On entry: rdi = arg, rsi = fn  (restored from initial stack frame)
; Calls fn(arg), then thread_exit.
;
global thread_run_trampoline
thread_run_trampoline:
    xor rbp, rbp        ; clear frame pointer for stack walks
    call rsi            ; fn(arg): rdi already holds arg
    call thread_exit    ; fn returned — exit gracefully
.hang:
    hlt
    jmp .hang

; ---- sched_idle_loop -----------------------------------------------------
;
; Per-CPU idle loop. Enables interrupts and halts, allowing the timer ISR
; to wake the CPU and switch to a ready thread.
;
global sched_idle_loop
sched_idle_loop:
    sti
    hlt
    jmp sched_idle_loop

section .note.GNU-stack noalloc noexec nowrite progbits
