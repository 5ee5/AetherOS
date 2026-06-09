; SYSCALL/SYSRET entry point.
;
; On SYSCALL entry the CPU has:
;   RCX  = saved user RIP
;   R11  = saved user RFLAGS
;   RSP  = still the user stack (NOT changed by hardware)
;   CS   = kernel code selector (from STAR[47:32])
;   SS   = kernel data selector (from STAR[47:32]+8)
;   RFLAGS IF cleared (masked by IA32_FMASK)
;
; Calling convention for syscall_dispatch:
;   nr  → RDI   (from user RAX)
;   a0  → RSI   (from user RDI)
;   a1  → RDX   (from user RSI)
;   a2  → RCX   (from user RDX) — careful: RCX holds saved RIP
;   a3  → R8    (from user R10, because user RCX is clobbered by SYSCALL)
;   a4  → R9    (from user R8)
; Return value in RAX.

bits 64

extern syscall_dispatch

global syscall_entry
syscall_entry:
    ; Swap GS: kernel GS base now visible (cpu_local_data[cpu])
    swapgs

    ; Save user RSP and load kernel RSP from cpu_local (offsets 8 and 0).
    ; Then immediately push user RSP onto the kernel stack so it is
    ; thread-local — cpu_local.user_rsp is per-CPU and would be clobbered
    ; if the scheduler preempts us and another thread makes a syscall.
    mov  [gs:8], rsp         ; cpu_local.user_rsp = user RSP (temp)
    mov  rsp, [gs:0]         ; rsp = cpu_local.kernel_rsp
    push QWORD [gs:8]        ; push user RSP onto kernel stack (thread-local)
    swapgs                   ; restore user GS (kernel doesn't need it)

    ; Now on the kernel stack. Save all caller-save + syscall-clobbered regs.
    ; We push a frame compatible with iretq so the timer ISR can preempt us
    ; while we're in the syscall handler.
    push rcx                 ; saved user RIP
    push r11                 ; saved user RFLAGS
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Build syscall_dispatch args:
    ;   rdi = nr (user rax)
    ;   rsi = a0 (user rdi)
    ;   rdx = a1 (user rsi)
    ;   rcx = a2 (user rdx) — NOTE: rcx was clobbered by SYSCALL; we already saved it
    ;   r8  = a3 (user r10, Linux ABI 4th arg uses r10 instead of rcx)
    ;   r9  = a4 (user r8)
    ;
    ; User arg regs on entry: rdi=a0, rsi=a1, rdx=a2, r10=a3, r8=a4, r9=a5
    ; Rearrange: nr from rax→rdi, then shift remaining args.
    mov  r9,  r8             ; a4 = user r8
    mov  r8,  r10            ; a3 = user r10
    mov  rcx, rdx            ; a2 = user rdx
    mov  rdx, rsi            ; a1 = user rsi
    mov  rsi, rdi            ; a0 = user rdi
    mov  rdi, rax            ; nr = user rax

    ; Re-enable interrupts while in the kernel syscall handler.
    sti

    call syscall_dispatch

    cli

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    pop  r11                 ; RFLAGS for sysretq
    pop  rcx                 ; RIP for sysretq

    ; Restore user RSP from the kernel stack (thread-local, not cpu_local).
    ; No swapgs needed — GS is already user GS (set by the second swapgs in
    ; the prologue and never changed again during dispatch).
    pop  rsp                 ; rsp = saved user RSP

    o64 sysret

section .note.GNU-stack noalloc noexec nowrite progbits
