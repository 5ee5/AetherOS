; SMP AP startup trampoline — assembled as a flat binary (nasm -f bin).
; Loaded at physical 0x8000 by the BSP before sending SIPI.
; SIPI vector 0x08 → AP starts at CS=0x0800, IP=0x0000 → phys 0x8000.
;
; Boot data layout (BSP fills before each SIPI):
;   phys 0x8F00  ap_pml4   (qword)  PML4 physical address
;   phys 0x8F08  ap_entry  (qword)  64-bit C entry point
;   phys 0x8F10  ap_stack  (qword)  AP stack top (per-AP, written before SIPI)
;   phys 0x8F18  ap_ready  (dword)  AP sets to 1 when in 64-bit mode

bits 16
org 0x8000

    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load temporary GDT
    lgdt [ap_gdtr]

    ; Enable protected mode (CR0.PE)
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; Far jump to flush pipeline and load 32-bit code selector (0x08)
    db 0x66, 0xEA           ; JMP FAR with 32-bit operand size
    dd trampoline32         ; target EIP
    dw 0x08                 ; 32-bit code selector

bits 32
trampoline32:
    mov ax, 0x10            ; data selector
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load PML4 (same as BSP) from boot data at 0x8F00
    mov eax, [0x8F00]
    mov cr3, eax

    ; Enable PAE
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; Enable EFER.LME (long mode enable)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; Enable paging (CR0.PG)
    mov eax, cr0
    or  eax, (1 << 31)
    mov cr0, eax

    ; Far jump to 64-bit code selector (0x18)
    db 0xEA
    dd trampoline64
    dw 0x18

bits 64
trampoline64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor rbp, rbp

    ; Load per-AP stack pointer
    mov rsp, [0x8F10]

    ; Signal BSP: AP has reached 64-bit mode
    mov dword [0x8F18], 1

    ; Jump to C entry point
    mov rax, [0x8F08]
    jmp rax

; ---- Pad to boot data area at trampoline offset 0x0F00 -----------------
times (0x0F00 - ($ - $$)) db 0

ap_pml4:    dq 0            ; 0x8F00
ap_entry:   dq 0            ; 0x8F08
ap_stack:   dq 0            ; 0x8F10
ap_ready:   dd 0            ; 0x8F18
            dd 0            ; 0x8F1C  padding

; Temporary GDT (null, code32, data, code64)
ap_gdt:
    dq 0x0000000000000000   ; [0x00] null
    dq 0x00CF9A000000FFFF   ; [0x08] 32-bit code: G=1, D/B=1, L=0
    dq 0x00CF92000000FFFF   ; [0x10] data: G=1, D/B=1
    dq 0x00AF9A000000FFFF   ; [0x18] 64-bit code: G=1, D/B=0, L=1

; GDTR (6 bytes: 2-byte limit + 4-byte base)
ap_gdtr:
    dw (ap_gdtr - ap_gdt - 1)   ; limit = 4*8 - 1 = 31
    dd ap_gdt                    ; base  = physical address of ap_gdt
