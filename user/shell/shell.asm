; AetherOS interactive shell
; Assembled with: nasm -f elf64 -o build/shell.o user/shell/shell.asm
; Linked with:    ld -m elf_x86_64 -Ttext=0x400000 -e _start --build-id=none -o build/shell.elf build/shell.o

BITS 64
DEFAULT REL

%define SYS_READ    0
%define SYS_WRITE   1
%define SYS_OPEN    2
%define SYS_CLOSE   3
%define SYS_PIPE    22
%define SYS_EXIT    60
%define SYS_WAITPID 61
%define SYS_GETCWD  79
%define SYS_CHDIR   80
%define SYS_SPAWN   500
%define SYS_LISTDIR 600

%define O_WRONLY    1
%define O_CREAT     0x40
%define O_TRUNC     0x200

%define LINE_MAX    256
%define CAT_BUF_SZ  4096
%define LS_BUF_SZ   4096
%define CWD_BUF_SZ  256
%define MAX_PIPE_SEGS 4

section .text
global _start

_start:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [banner]
    mov rdx, banner_len
    syscall

.prompt_loop:
    mov rax, SYS_GETCWD
    lea rdi, [cwd_buf]
    mov rsi, CWD_BUF_SZ
    syscall

    lea rsi, [cwd_buf]
    call writestr

    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [prompt_suffix]
    mov rdx, prompt_suffix_len
    syscall

    call readline
    test rax, rax
    jz .prompt_loop

    call execute
    jmp .prompt_loop

; ---- readline ---------------------------------------------------------------
; Returns: rax = chars read, line_buf NUL-terminated (newline stripped).

readline:
    push rbx
    push r12
    xor rbx, rbx

.rl_char:
    mov rax, SYS_READ
    xor rdi, rdi
    lea rsi, [ch_buf]
    mov rdx, 1
    syscall
    test rax, rax
    jle .rl_done

    movzx rax, byte [ch_buf]
    cmp rax, 0x08
    je .rl_bs
    cmp rax, 0x7f
    je .rl_bs
    cmp rax, 10
    je .rl_nl
    cmp rax, 13
    je .rl_nl
    cmp rax, 32
    jb .rl_char
    cmp rbx, LINE_MAX - 1
    jge .rl_char
    mov byte [line_buf + rbx], al
    inc rbx
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [ch_buf]
    mov rdx, 1
    syscall
    jmp .rl_char

.rl_bs:
    test rbx, rbx
    jz .rl_char
    dec rbx
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [bs_seq]
    mov rdx, 3
    syscall
    jmp .rl_char

.rl_nl:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [nl]
    mov rdx, 1
    syscall
    mov byte [line_buf + rbx], 0
    mov rax, rbx
.rl_done:
    pop r12
    pop rbx
    ret

; ---- strlen -----------------------------------------------------------------
; Input:  rdi = string  Output: rcx = length

strlen:
    xor rcx, rcx
.sl_lp:
    cmp byte [rdi + rcx], 0
    je .sl_done
    inc rcx
    jmp .sl_lp
.sl_done:
    ret

; ---- streq ------------------------------------------------------------------
; Input: rdi = s1, rsi = s2  Output: ZF set if equal

streq:
    xor rcx, rcx
.sq_lp:
    mov al, [rdi + rcx]
    cmp al, [rsi + rcx]
    jne .sq_ne
    test al, al
    jz .sq_eq
    inc rcx
    jmp .sq_lp
.sq_eq:
    xor eax, eax
    ret
.sq_ne:
    mov eax, 1
    test eax, eax
    ret

; ---- writestr ---------------------------------------------------------------
; Input: rsi = NUL-terminated string

writestr:
    mov rdi, rsi
    call strlen
    test rcx, rcx
    jz .ws_done
    mov rax, SYS_WRITE
    mov rdi, 1
    mov rdx, rcx
    syscall
.ws_done:
    ret

; ---- spawn_segment ----------------------------------------------------------
; Parse and spawn a command line segment.
; Input:  rbx = start of NUL-terminated command line (tokens in line_buf)
;         rdx = stdin_fd  override (-1 = default keyboard)
;         r10 = stdout_fd override (-1 = default serial)
; Output: rax = pid (negative on error)
; Clobbers: rcx (and internally r12/r13/r14/rbx, saved/restored)

spawn_segment:
    push r12
    push r13
    push r14
    push rbx
    push rdx        ; save stdin_fd  (syscall will clobber rdx)
    push r10        ; save stdout_fd (syscall will clobber r10)

    cmp byte [rbx], 0
    jz .ss_empty

    ; Parse command word
    mov r12, rbx
    mov r13, rbx
.ss_findend:
    mov al, [r13]
    test al, al
    jz .ss_noargs
    cmp al, ' '
    je .ss_gotsp
    inc r13
    jmp .ss_findend
.ss_gotsp:
    mov byte [r13], 0
    inc r13
.ss_skipsp:
    cmp byte [r13], ' '
    jne .ss_haveargs
    inc r13
    jmp .ss_skipsp
.ss_noargs:
.ss_haveargs:
    ; Tokenize remaining args into spawn_argv[]
    mov [spawn_argv], r12
    xor r14, r14
    mov rbx, r13
.ss_sp:
    cmp byte [rbx], ' '
    jne .ss_tok
    inc rbx
    jmp .ss_sp
.ss_tok:
    cmp byte [rbx], 0
    je .ss_term
    cmp r14, 8
    jge .ss_term
    lea rcx, [spawn_argv + 8]
    mov [rcx + r14*8], rbx
    inc r14
.ss_tokend:
    cmp byte [rbx], 0
    je .ss_term
    cmp byte [rbx], ' '
    je .ss_nulltok
    inc rbx
    jmp .ss_tokend
.ss_nulltok:
    mov byte [rbx], 0
    inc rbx
    jmp .ss_sp
.ss_term:
    lea rcx, [spawn_argv + 8]
    mov qword [rcx + r14*8], 0

    pop r10         ; restore stdout_fd
    pop rdx         ; restore stdin_fd
    mov rax, SYS_SPAWN
    mov rdi, r12
    lea rsi, [spawn_argv]
    syscall
    jmp .ss_done

.ss_empty:
    pop r10
    pop rdx
    mov rax, -1

.ss_done:
    pop rbx
    pop r14
    pop r13
    pop r12
    ret

; ---- execute ----------------------------------------------------------------

execute:
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Clear background flag
    mov byte [bg_flag], 0

    ; Skip leading spaces
    lea rbx, [line_buf]
.skip_sp:
    mov al, [rbx]
    test al, al
    jz .ex_done
    cmp al, ' '
    jne .scan_amp
    inc rbx
    jmp .skip_sp

    ; === BACKGROUND DETECTION: scan from end for trailing '&' ===
.scan_amp:
    mov rcx, rbx
.amp_end:
    cmp byte [rcx], 0
    je .amp_back
    inc rcx
    jmp .amp_end
.amp_back:
    dec rcx     ; point to last char
.amp_skip_trail_sp:
    cmp rcx, rbx
    jb .amp_done
    cmp byte [rcx], ' '
    jne .amp_check_char
    dec rcx
    jmp .amp_skip_trail_sp
.amp_check_char:
    cmp byte [rcx], '&'
    jne .amp_done
    mov byte [rcx], 0
    mov byte [bg_flag], 1
    ; Trim any spaces left before the '&'
    dec rcx
.amp_trim2:
    cmp rcx, rbx
    jb .amp_done
    cmp byte [rcx], ' '
    jne .amp_done
    mov byte [rcx], 0
    dec rcx
    jmp .amp_trim2
.amp_done:
    ; Check if line became empty after stripping '&'
    cmp byte [rbx], 0
    je .ex_done

    ; === PIPE DETECTION: count '|' chars, record segment starts ===
    ; Store first segment start
    mov qword [pipe_seg_ptrs], rbx
    mov qword [pipe_seg_count], 1
    mov rcx, rbx

.pipe_scan:
    mov al, [rcx]
    test al, al
    jz .pipe_scan_done
    cmp al, '|'
    je .pipe_found_bar
    inc rcx
    jmp .pipe_scan

.pipe_found_bar:
    mov r15, [pipe_seg_count]
    cmp r15, MAX_PIPE_SEGS
    jge .pipe_scan_skip     ; too many segments, ignore extra |

    ; Trim spaces to the left of '|'
    lea rdx, [rcx - 1]
.pipe_trim_left:
    cmp rdx, rbx
    jbe .pipe_trim_done
    cmp byte [rdx], ' '
    jne .pipe_trim_done
    dec rdx
    jmp .pipe_trim_left
.pipe_trim_done:
    inc rdx
    mov byte [rdx], 0       ; null-terminate left segment

    ; Skip spaces after '|'
    inc rcx
.pipe_skip_right_sp:
    cmp byte [rcx], ' '
    jne .pipe_store_seg
    inc rcx
    jmp .pipe_skip_right_sp
.pipe_store_seg:
    mov r15, [pipe_seg_count]
    mov [pipe_seg_ptrs + r15*8], rcx
    inc r15
    mov [pipe_seg_count], r15
.pipe_scan_skip:
    inc rcx
    jmp .pipe_scan

.pipe_scan_done:
    ; r15 / pipe_seg_count = number of segments

    mov r15, [pipe_seg_count]
    cmp r15, 1
    je .single_cmd           ; no pipes: normal flow

    ; ============================================================
    ; MULTI-PIPE FLOW
    ; ============================================================
    ; Create r15-1 pipes
    mov r14, r15
    dec r14                  ; r14 = pipe count needed
    xor r13, r13             ; r13 = pipe index
.mp_create:
    cmp r13, r14
    jge .mp_created
    lea rdi, [pipe_fd_array]
    mov rcx, r13
    shl rcx, 3
    add rdi, rcx             ; rdi = &pipe_fd_array[r13]
    mov rax, SYS_PIPE
    syscall
    test rax, rax
    js .pipe_err
    inc r13
    jmp .mp_create
.mp_created:

    ; Spawn all segments
    mov r15, [pipe_seg_count] ; r15 = seg count
    xor r13, r13              ; r13 = segment index
.mp_spawn:
    cmp r13, r15
    jge .mp_spawned
    mov rbx, [pipe_seg_ptrs + r13*8]

    ; stdin_fd: first segment → -1, else pipe[r13-1].read_fd
    test r13, r13
    jz .mp_stdin_def
    lea rax, [pipe_fd_array]
    mov rcx, r13
    dec rcx
    shl rcx, 3
    add rax, rcx
    movsx rdx, dword [rax]
    jmp .mp_stdin_set
.mp_stdin_def:
    mov rdx, -1
.mp_stdin_set:

    ; stdout_fd: last segment → -1, else pipe[r13].write_fd
    mov rcx, r15
    dec rcx
    cmp r13, rcx
    je .mp_stdout_def
    lea rax, [pipe_fd_array]
    mov rcx, r13
    shl rcx, 3
    add rax, rcx
    movsx r10, dword [rax + 4]
    jmp .mp_stdout_set
.mp_stdout_def:
    mov r10, -1
.mp_stdout_set:

    call spawn_segment
    mov [pipe_pids + r13*8], rax
    inc r13
    jmp .mp_spawn
.mp_spawned:

    ; Close all parent pipe fd copies
    mov r14, [pipe_seg_count]
    dec r14                   ; r14 = pipe count
    xor r13, r13
.mp_close:
    cmp r13, r14
    jge .mp_closed
    lea rbx, [pipe_fd_array]
    mov rcx, r13
    shl rcx, 3
    add rbx, rcx
    movsx rdi, dword [rbx]    ; close read end
    mov rax, SYS_CLOSE
    syscall
    movsx rdi, dword [rbx + 4]; close write end
    mov rax, SYS_CLOSE
    syscall
    inc r13
    jmp .mp_close
.mp_closed:

    cmp byte [bg_flag], 0
    jne .ex_done

    ; Waitpid all children (reverse order: last first)
    mov r13, [pipe_seg_count]
    dec r13
.mp_wait:
    cmp r13, 0
    jl .ex_done
    mov rdi, [pipe_pids + r13*8]
    test rdi, rdi
    js .mp_wait_next
    mov rax, SYS_WAITPID
    xor rsi, rsi
    xor rdx, rdx
    syscall
.mp_wait_next:
    dec r13
    jmp .mp_wait

.pipe_err:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [err_pipe]
    mov rdx, err_pipe_len
    syscall
    jmp .ex_done

    ; ============================================================
    ; SINGLE COMMAND FLOW
    ; ============================================================
.single_cmd:
    ; rbx = start of command line (NUL-terminated, no pipe chars)
    mov r12, rbx        ; r12 = cmd word start
    mov r13, rbx

.find_end:
    mov al, [r13]
    test al, al
    jz .no_args
    cmp al, ' '
    je .found_sp
    inc r13
    jmp .find_end
.found_sp:
    mov byte [r13], 0
    inc r13
.skip_arg_sp:
    mov al, [r13]
    cmp al, ' '
    jne .have_args
    inc r13
    jmp .skip_arg_sp
.have_args:
    jmp .compare
.no_args:
    jmp .compare

    ; ---- Built-in: echo ----
.compare:
    lea rsi, [cmd_echo]
    mov rdi, r12
    call streq
    jnz .try_cat

    mov rcx, r13
.echo_scan_gt:
    mov al, [rcx]
    test al, al
    jz .echo_no_redir
    cmp al, '>'
    je .echo_found_gt
    inc rcx
    jmp .echo_scan_gt

.echo_found_gt:
    mov rbx, rcx
    lea rdx, [rcx - 1]
.echo_rtrim:
    cmp rdx, r13
    jbe .echo_rtrim_done
    cmp byte [rdx], ' '
    jne .echo_rtrim_done
    dec rdx
    jmp .echo_rtrim
.echo_rtrim_done:
    inc rdx
    mov byte [rdx], 0

    inc rbx
.echo_skip_sp_path:
    cmp byte [rbx], ' '
    jne .echo_open_file
    inc rbx
    jmp .echo_skip_sp_path
.echo_open_file:
    mov rax, SYS_OPEN
    mov rdi, rbx
    mov rsi, O_WRONLY | O_CREAT | O_TRUNC
    mov rdx, 0644
    syscall
    cmp rax, 0
    jl .echo_redir_fail
    mov r14, rax
    mov rsi, r13
    mov rdi, rsi
    call strlen
    test rcx, rcx
    jz .echo_redir_nl
    mov rax, SYS_WRITE
    mov rdi, r14
    mov rdx, rcx
    syscall
.echo_redir_nl:
    mov rax, SYS_WRITE
    mov rdi, r14
    lea rsi, [nl]
    mov rdx, 1
    syscall
    mov rax, SYS_CLOSE
    mov rdi, r14
    syscall
    jmp .ex_done
.echo_redir_fail:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [err_redir]
    mov rdx, err_redir_len
    syscall
    jmp .ex_done
.echo_no_redir:
    mov rsi, r13
    call writestr
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [nl]
    mov rdx, 1
    syscall
    jmp .ex_done

    ; ---- Built-in: cat ----
.try_cat:
    lea rsi, [cmd_cat]
    mov rdi, r12
    call streq
    jnz .try_ls
    mov al, [r13]
    test al, al
    jz .ex_done
    mov rax, SYS_OPEN
    mov rdi, r13
    xor rsi, rsi
    xor rdx, rdx
    syscall
    cmp rax, 0
    jl .err_notfound
    mov r14, rax
.cat_loop:
    mov rax, SYS_READ
    mov rdi, r14
    lea rsi, [cat_buf]
    mov rdx, CAT_BUF_SZ
    syscall
    test rax, rax
    jle .cat_close
    mov rdx, rax
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [cat_buf]
    syscall
    jmp .cat_loop
.cat_close:
    mov rax, SYS_CLOSE
    mov rdi, r14
    syscall
    jmp .ex_done

    ; ---- Built-in: ls ----
.try_ls:
    lea rsi, [cmd_ls]
    mov rdi, r12
    call streq
    jnz .try_cd
    mov al, [r13]
    test al, al
    jnz .ls_path
    mov rax, SYS_GETCWD
    lea rdi, [cwd_buf]
    mov rsi, CWD_BUF_SZ
    syscall
    lea r13, [cwd_buf]
.ls_path:
    mov rax, SYS_LISTDIR
    mov rdi, r13
    lea rsi, [ls_buf]
    mov rdx, LS_BUF_SZ
    syscall
    test rax, rax
    jle .ex_done
    mov rdx, rax
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [ls_buf]
    syscall
    jmp .ex_done

    ; ---- Built-in: cd ----
.try_cd:
    lea rsi, [cmd_cd]
    mov rdi, r12
    call streq
    jnz .try_pwd
    mov al, [r13]
    test al, al
    jnz .cd_path
    lea r13, [root_path]
.cd_path:
    mov rax, SYS_CHDIR
    mov rdi, r13
    syscall
    test rax, rax
    jz .ex_done
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [err_chdir]
    mov rdx, err_chdir_len
    syscall
    jmp .ex_done

    ; ---- Built-in: pwd ----
.try_pwd:
    lea rsi, [cmd_pwd]
    mov rdi, r12
    call streq
    jnz .try_clear
    mov rax, SYS_GETCWD
    lea rdi, [cwd_buf]
    mov rsi, CWD_BUF_SZ
    syscall
    lea rsi, [cwd_buf]
    call writestr
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [nl]
    mov rdx, 1
    syscall
    jmp .ex_done

    ; ---- Built-in: clear ----
.try_clear:
    lea rsi, [cmd_clear]
    mov rdi, r12
    call streq
    jnz .try_uname
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [clear_seq]
    mov rdx, clear_seq_len
    syscall
    jmp .ex_done

    ; ---- Built-in: uname ----
.try_uname:
    lea rsi, [cmd_uname]
    mov rdi, r12
    call streq
    jnz .try_help
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [uname_str]
    mov rdx, uname_str_len
    syscall
    jmp .ex_done

    ; ---- Built-in: help ----
.try_help:
    lea rsi, [cmd_help]
    mov rdi, r12
    call streq
    jnz .try_exit
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [help_text]
    mov rdx, help_text_len
    syscall
    jmp .ex_done

    ; ---- Built-in: exit ----
.try_exit:
    lea rsi, [cmd_exit]
    mov rdi, r12
    call streq
    jnz .try_spawn
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

    ; ---- External spawn ----
.try_spawn:
    ; Tokenize args (r13 = args start, r12 = cmd)
    mov [spawn_argv + 0], r12
    xor r14, r14
    mov rbx, r13
.sp_skip_sp:
    cmp byte [rbx], ' '
    jne .sp_tok
    inc rbx
    jmp .sp_skip_sp
.sp_tok:
    cmp byte [rbx], 0
    je .sp_term
    cmp r14, 8
    jge .sp_term
    lea rcx, [spawn_argv + 8]
    mov [rcx + r14*8], rbx
    inc r14
.sp_tokend:
    cmp byte [rbx], 0
    je .sp_term
    cmp byte [rbx], ' '
    je .sp_nulltok
    inc rbx
    jmp .sp_tokend
.sp_nulltok:
    mov byte [rbx], 0
    inc rbx
    jmp .sp_skip_sp
.sp_term:
    lea rcx, [spawn_argv + 8]
    mov qword [rcx + r14*8], 0
    mov rax, SYS_SPAWN
    mov rdi, r12
    lea rsi, [spawn_argv]
    mov rdx, -1
    mov r10, -1
    syscall
    cmp rax, 0
    jl .err_notfound
    cmp byte [bg_flag], 0
    jne .ex_done        ; background: don't wait
    mov rdi, rax
    mov rax, SYS_WAITPID
    xor rsi, rsi
    xor rdx, rdx
    syscall
    jmp .ex_done

.err_notfound:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [err_notfound]
    mov rdx, err_notfound_len
    syscall
    jmp .ex_done

.ex_done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; ---- Data -------------------------------------------------------------------

section .data

banner:     db "AetherOS shell — type 'help' for commands", 10
banner_len  equ $ - banner

prompt_suffix:      db " $ "
prompt_suffix_len   equ $ - prompt_suffix

nl:         db 10
bs_seq:     db 8, ' ', 8

clear_seq:      db 27, '[', '2', 'J', 27, '[', 'H'
clear_seq_len   equ $ - clear_seq

uname_str:      db "AetherOS 1.0 x86_64 (hobby kernel)", 10
uname_str_len   equ $ - uname_str

help_text:      db "Built-in commands:", 10
                db "  echo [text]   - print text (supports > redirection)", 10
                db "  cat <path>    - print file contents", 10
                db "  ls [path]     - list directory", 10
                db "  cd [path]     - change directory", 10
                db "  pwd           - print working directory", 10
                db "  clear         - clear screen", 10
                db "  uname         - print OS info", 10
                db "  help          - this help", 10
                db "  exit          - quit shell", 10
                db "Pipes:     cmd1 | cmd2 | cmd3  (up to 4 stages)", 10
                db "Background: cmd &", 10
                db "Ctrl+C:    kill foreground process", 10
help_text_len   equ $ - help_text

err_notfound:       db "command not found", 10
err_notfound_len    equ $ - err_notfound

err_chdir:      db "cd: no such directory", 10
err_chdir_len   equ $ - err_chdir

err_redir:      db "echo: cannot open output file", 10
err_redir_len   equ $ - err_redir

err_pipe:       db "pipe: failed", 10
err_pipe_len    equ $ - err_pipe

cmd_echo:   db "echo", 0
cmd_cat:    db "cat", 0
cmd_ls:     db "ls", 0
cmd_cd:     db "cd", 0
cmd_pwd:    db "pwd", 0
cmd_clear:  db "clear", 0
cmd_uname:  db "uname", 0
cmd_help:   db "help", 0
cmd_exit:   db "exit", 0
root_path:  db "/", 0

section .bss

line_buf:        resb LINE_MAX
ch_buf:          resb 1
cat_buf:         resb CAT_BUF_SZ
ls_buf:          resb LS_BUF_SZ
cwd_buf:         resb CWD_BUF_SZ
spawn_argv:      resq 10
bg_flag:         resb 1
pipe_seg_count:  resq 1
pipe_seg_ptrs:   resq 4          ; up to 4 segment start pointers
pipe_fd_array:   resd 6          ; 3 pipes × 2 int32 fds
pipe_pids:       resq 4          ; PID for each segment

section .note.GNU-stack noalloc noexec nowrite progbits
