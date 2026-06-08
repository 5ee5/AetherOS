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

; open flags
%define O_WRONLY    1
%define O_CREAT     0x40
%define O_TRUNC     0x200

%define LINE_MAX    256
%define CAT_BUF_SZ  4096
%define LS_BUF_SZ   4096
%define CWD_BUF_SZ  256

section .text
global _start

_start:
    ; Print banner
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [banner]
    mov rdx, banner_len
    syscall

.prompt_loop:
    ; Print cwd + " $ " prompt
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

    ; Read a line into line_buf
    call readline           ; returns length in rax
    test rax, rax
    jz .prompt_loop

    ; Execute the command
    call execute

    jmp .prompt_loop

; ---- readline ---------------------------------------------------------------
; Reads from fd=0 one char at a time until '\n'.
; Echoes printable characters and handles backspace (0x08, 0x7f).
; Returns: rax = number of chars in line_buf (without '\n'), buf is NUL-terminated.

readline:
    push rbx
    push r12
    xor rbx, rbx       ; rbx = current length

.read_char:
    ; sys_read(0, &ch_buf, 1)
    mov rax, SYS_READ
    xor rdi, rdi        ; fd = 0
    lea rsi, [ch_buf]
    mov rdx, 1
    syscall
    test rax, rax
    jle .done_read

    movzx rax, byte [ch_buf]

    ; Backspace (0x08) or Delete (0x7f)?
    cmp rax, 0x08
    je .backspace
    cmp rax, 0x7f
    je .backspace

    ; Newline? (LF or CR — kernel normalizes CR to LF, but handle both)
    cmp rax, 10
    je .newline
    cmp rax, 13
    je .newline

    ; Ignore other control chars
    cmp rax, 32
    jb .read_char

    ; Buffer full?
    cmp rbx, LINE_MAX - 1
    jge .read_char

    ; Store and echo the character
    mov byte [line_buf + rbx], al
    inc rbx

    ; Echo: sys_write(1, &ch_buf, 1)
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [ch_buf]
    mov rdx, 1
    syscall
    jmp .read_char

.backspace:
    test rbx, rbx
    jz .read_char       ; nothing to delete
    dec rbx
    ; Write backspace-space-backspace to erase character on terminal
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [bs_seq]
    mov rdx, 3
    syscall
    jmp .read_char

.newline:
    ; Echo the newline
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [nl]
    mov rdx, 1
    syscall
    ; NUL-terminate
    mov byte [line_buf + rbx], 0
    mov rax, rbx

.done_read:
    pop r12
    pop rbx
    ret

; ---- strlen -----------------------------------------------------------------
; Input:  rdi = pointer to NUL-terminated string
; Output: rcx = length

strlen:
    xor rcx, rcx
.lp:
    cmp byte [rdi + rcx], 0
    je .done
    inc rcx
    jmp .lp
.done:
    ret

; ---- streq ------------------------------------------------------------------
; Input:  rdi = str1, rsi = str2
; Output: ZF set if equal
; Clobbers: rax, rcx

streq:
    xor rcx, rcx
.lp:
    mov al, [rdi + rcx]
    cmp al, [rsi + rcx]
    jne .ne
    test al, al
    jz .eq
    inc rcx
    jmp .lp
.eq:
    xor eax, eax
    ret
.ne:
    mov eax, 1
    test eax, eax
    ret

; ---- writestr ---------------------------------------------------------------
; Input:  rsi = NUL-terminated string
; Clobbers: rax, rdi, rdx, rcx

writestr:
    mov rdi, rsi
    call strlen         ; rcx = length
    test rcx, rcx
    jz .done
    mov rax, SYS_WRITE
    mov rdi, 1
    ; rsi already set
    mov rdx, rcx
    syscall
.done:
    ret

; ---- execute ----------------------------------------------------------------
; Parses line_buf, checks built-ins, else sys_spawn + sys_waitpid.

execute:
    push rbx
    push r12
    push r13
    push r14

    ; Skip leading spaces
    lea rbx, [line_buf]
.skip_spaces:
    mov al, [rbx]
    test al, al
    jz .ex_done         ; empty line
    cmp al, ' '
    jne .scan_for_pipe
    inc rbx
    jmp .skip_spaces

    ; === PIPE SCAN: look for '|' before any arg processing ===
.scan_for_pipe:
    mov rcx, rbx
.pipe_scan:
    mov al, [rcx]
    test al, al
    jz .find_arg        ; no pipe found
    cmp al, '|'
    je .pipe_found
    inc rcx
    jmp .pipe_scan

.pipe_found:
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
    mov byte [rdx], 0           ; null-terminate left side

    ; Skip spaces after '|' to find right side start
    inc rcx
.pipe_skip_right:
    cmp byte [rcx], ' '
    jne .pipe_right_ok
    inc rcx
    jmp .pipe_skip_right
.pipe_right_ok:
    ; Check both sides are non-empty
    cmp byte [rbx], 0
    je .pipe_bad
    cmp byte [rcx], 0
    je .pipe_bad

    ; Save right side pointer
    mov [pipe_right_ptr], rcx

    ; sys_pipe(&pipe_fds)
    mov rax, SYS_PIPE
    lea rdi, [pipe_fds]
    syscall
    test rax, rax
    js .pipe_bad

    ; ---- Spawn LEFT command (stdout → pipe write end) ----
    ; rbx = start of left command line (already NUL-terminated)
    ; Parse cmd word
    mov r12, rbx
    mov r13, rbx
.pleft_findend:
    mov al, [r13]
    test al, al
    jz .pleft_noargs
    cmp al, ' '
    je .pleft_gotsp
    inc r13
    jmp .pleft_findend
.pleft_gotsp:
    mov byte [r13], 0
    inc r13
.pleft_skipsp:
    cmp byte [r13], ' '
    jne .pleft_haveargs
    inc r13
    jmp .pleft_skipsp
.pleft_noargs:
.pleft_haveargs:
    ; Tokenize left args into spawn_argv[]
    mov [spawn_argv], r12
    xor r14, r14
    mov rbx, r13
.pleft_sp:
    cmp byte [rbx], ' '
    jne .pleft_tok
    inc rbx
    jmp .pleft_sp
.pleft_tok:
    cmp byte [rbx], 0
    je .pleft_term
    cmp r14, 8
    jge .pleft_term
    lea rcx, [spawn_argv + 8]
    mov [rcx + r14*8], rbx
    inc r14
.pleft_tokend:
    cmp byte [rbx], 0
    je .pleft_term
    cmp byte [rbx], ' '
    je .pleft_nulltok
    inc rbx
    jmp .pleft_tokend
.pleft_nulltok:
    mov byte [rbx], 0
    inc rbx
    jmp .pleft_sp
.pleft_term:
    lea rcx, [spawn_argv + 8]
    mov qword [rcx + r14*8], 0
    ; sys_spawn(path, argv, stdin=-1, stdout=pipe_fds[1])
    mov rax, SYS_SPAWN
    mov rdi, r12
    lea rsi, [spawn_argv]
    mov rdx, -1                        ; stdin = default keyboard
    movsx r10, dword [pipe_fds + 4]    ; stdout = pipe write end
    syscall
    mov [pipe_left_pid], rax

    ; ---- Spawn RIGHT command (stdin → pipe read end) ----
    mov rbx, [pipe_right_ptr]
    mov r12, rbx
    mov r13, rbx
.pright_findend:
    mov al, [r13]
    test al, al
    jz .pright_noargs
    cmp al, ' '
    je .pright_gotsp
    inc r13
    jmp .pright_findend
.pright_gotsp:
    mov byte [r13], 0
    inc r13
.pright_skipsp:
    cmp byte [r13], ' '
    jne .pright_haveargs
    inc r13
    jmp .pright_skipsp
.pright_noargs:
.pright_haveargs:
    ; Tokenize right args into spawn_argv[]
    mov [spawn_argv], r12
    xor r14, r14
    mov rbx, r13
.pright_sp:
    cmp byte [rbx], ' '
    jne .pright_tok
    inc rbx
    jmp .pright_sp
.pright_tok:
    cmp byte [rbx], 0
    je .pright_term
    cmp r14, 8
    jge .pright_term
    lea rcx, [spawn_argv + 8]
    mov [rcx + r14*8], rbx
    inc r14
.pright_tokend:
    cmp byte [rbx], 0
    je .pright_term
    cmp byte [rbx], ' '
    je .pright_nulltok
    inc rbx
    jmp .pright_tokend
.pright_nulltok:
    mov byte [rbx], 0
    inc rbx
    jmp .pright_sp
.pright_term:
    lea rcx, [spawn_argv + 8]
    mov qword [rcx + r14*8], 0
    ; sys_spawn(path, argv, stdin=pipe_fds[0], stdout=-1)
    mov rax, SYS_SPAWN
    mov rdi, r12
    lea rsi, [spawn_argv]
    movsx rdx, dword [pipe_fds]        ; stdin = pipe read end
    mov r10, -1                        ; stdout = default serial
    syscall
    mov [pipe_right_pid], rax

    ; Close parent's copies of the pipe fds
    movsx rdi, dword [pipe_fds]        ; close read end
    mov rax, SYS_CLOSE
    syscall
    movsx rdi, dword [pipe_fds + 4]    ; close write end
    mov rax, SYS_CLOSE
    syscall

    ; Wait for right child (consumer)
    mov rdi, [pipe_right_pid]
    test rdi, rdi
    js .pipe_wait_left
    mov rax, SYS_WAITPID
    xor rsi, rsi
    xor rdx, rdx
    syscall

.pipe_wait_left:
    ; Wait for left child (producer)
    mov rdi, [pipe_left_pid]
    test rdi, rdi
    js .ex_done
    mov rax, SYS_WAITPID
    xor rsi, rsi
    xor rdx, rdx
    syscall
    jmp .ex_done

.pipe_bad:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [err_pipe]
    mov rdx, err_pipe_len
    syscall
    jmp .ex_done

    ; === Normal (non-pipe) command parsing ===
.find_arg:
    ; rbx = start of command word
    ; Find end of first word, note start of args
    mov r12, rbx        ; r12 = cmd start
    mov r13, rbx        ; r13 will be args start (after cmd)
.find_end:
    mov al, [r13]
    test al, al
    jz .no_args
    cmp al, ' '
    je .found_space
    inc r13
    jmp .find_end

.found_space:
    ; Null-terminate the command word temporarily
    mov byte [r13], 0
    inc r13
    ; Skip additional spaces in args
.skip_arg_spaces:
    mov al, [r13]
    cmp al, ' '
    jne .have_args
    inc r13
    jmp .skip_arg_spaces
.have_args:
    jmp .compare

.no_args:
    ; r13 points to the NUL at end
    jmp .compare

.compare:
    ; --- echo ---
    lea rsi, [cmd_echo]
    mov rdi, r12
    call streq
    jnz .try_cat

    ; Scan args for " > path" redirection.
    ; r13 = start of args; scan for '>' byte.
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
    ; Null-terminate the text portion (byte before '>', strip trailing space).
    mov rbx, rcx      ; rbx = ptr to '>'
    ; walk back over spaces before '>'
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
    mov byte [rdx], 0   ; null-terminate text

    ; Advance past '>' and spaces to get filename.
    inc rbx
.echo_skip_spaces_path:
    cmp byte [rbx], ' '
    jne .echo_open_file
    inc rbx
    jmp .echo_skip_spaces_path

.echo_open_file:
    ; Open (or create+truncate) the output file.
    mov rax, SYS_OPEN
    mov rdi, rbx
    mov rsi, O_WRONLY | O_CREAT | O_TRUNC
    mov rdx, 0644
    syscall
    cmp rax, 0
    jl .echo_redir_fail
    mov r14, rax        ; r14 = output fd

    ; Write args to file.
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
    ; Close the file.
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
    ; No redirection — write args + newline to stdout.
    mov rsi, r13
    call writestr
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [nl]
    mov rdx, 1
    syscall
    jmp .ex_done

.try_cat:
    lea rsi, [cmd_cat]
    mov rdi, r12
    call streq
    jnz .try_ls
    ; cat <path>: open, read, write, close
    mov al, [r13]
    test al, al
    jz .ex_done         ; no argument
    mov rax, SYS_OPEN
    mov rdi, r13
    xor rsi, rsi
    xor rdx, rdx
    syscall
    cmp rax, 0
    jl .cat_notfound
    mov r14, rax        ; r14 = fd
.cat_read_loop:
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
    jmp .cat_read_loop
.cat_close:
    mov rax, SYS_CLOSE
    mov rdi, r14
    syscall
    jmp .ex_done
.cat_notfound:
    mov rsi, err_notfound
    call writestr
    jmp .ex_done

.try_ls:
    lea rsi, [cmd_ls]
    mov rdi, r12
    call streq
    jnz .try_cd
    ; ls [path] — default to cwd
    mov al, [r13]
    test al, al
    jnz .ls_with_path
    ; Use current working directory
    mov rax, SYS_GETCWD
    lea rdi, [cwd_buf]
    mov rsi, CWD_BUF_SZ
    syscall
    lea r13, [cwd_buf]
.ls_with_path:
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

.try_cd:
    lea rsi, [cmd_cd]
    mov rdi, r12
    call streq
    jnz .try_pwd
    ; cd [path] — default to /
    mov al, [r13]
    test al, al
    jnz .cd_with_path
    lea r13, [root_path]
.cd_with_path:
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

.try_exit:
    lea rsi, [cmd_exit]
    mov rdi, r12
    call streq
    jnz .try_spawn
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

.try_spawn:
    ; Tokenize remaining args in line_buf and build argv[].
    mov [spawn_argv + 0], r12      ; argv[0] = command name
    xor r14, r14                   ; r14 = arg count (for indices 1..8)
    mov rbx, r13                   ; rbx = scanner
.spawn_skip_sp:
    cmp byte [rbx], ' '
    jne .spawn_tok_start
    inc rbx
    jmp .spawn_skip_sp
.spawn_tok_start:
    cmp byte [rbx], 0
    je .spawn_term
    cmp r14, 8
    jge .spawn_term
    lea rcx, [spawn_argv + 8]
    mov [rcx + r14*8], rbx
    inc r14
.spawn_tok_end:
    cmp byte [rbx], 0
    je .spawn_term
    cmp byte [rbx], ' '
    je .spawn_null_tok
    inc rbx
    jmp .spawn_tok_end
.spawn_null_tok:
    mov byte [rbx], 0
    inc rbx
    jmp .spawn_skip_sp
.spawn_term:
    lea rcx, [spawn_argv + 8]
    mov qword [rcx + r14*8], 0     ; NULL-terminate argv
.do_spawn:
    mov rax, SYS_SPAWN
    mov rdi, r12
    lea rsi, [spawn_argv]
    mov rdx, -1                    ; stdin = default
    mov r10, -1                    ; stdout = default
    syscall
    cmp rax, 0
    jl .spawn_fail
    mov rdi, rax
    mov rax, SYS_WAITPID
    xor rsi, rsi
    xor rdx, rdx
    syscall
    jmp .ex_done

.spawn_fail:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [err_notfound]
    mov rdx, err_notfound_len
    syscall

.ex_done:
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; ---- Data -------------------------------------------------------------------

section .data

banner:     db "AetherOS shell v1.0 — type 'help' for commands", 10
banner_len  equ $ - banner

prompt_suffix:      db " $ "
prompt_suffix_len   equ $ - prompt_suffix

nl:         db 10
bs_seq:     db 8, ' ', 8       ; backspace erase sequence

clear_seq:      db 27, '[', '2', 'J', 27, '[', 'H'
clear_seq_len   equ $ - clear_seq

uname_str:      db "AetherOS 1.0 x86_64 (hobby kernel)", 10
uname_str_len   equ $ - uname_str

help_text:      db "Built-in commands:", 10
                db "  echo [text]   - print text", 10
                db "  cat <path>    - print file contents", 10
                db "  ls [path]     - list directory (default cwd)", 10
                db "  cd [path]     - change directory (default /)", 10
                db "  pwd           - print working directory", 10
                db "  clear         - clear screen", 10
                db "  uname         - print OS information", 10
                db "  help          - show this help", 10
                db "  exit          - quit shell", 10
                db "External:  <path>  - run ELF from filesystem", 10
                db "Pipes:     cmd1 | cmd2  - connect stdout to stdin", 10
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

line_buf:       resb LINE_MAX
ch_buf:         resb 1
cat_buf:        resb CAT_BUF_SZ
ls_buf:         resb LS_BUF_SZ
cwd_buf:        resb CWD_BUF_SZ
spawn_argv:     resq 10
pipe_fds:       resd 2          ; int[2]: [0]=read fd, [1]=write fd
pipe_left_pid:  resq 1
pipe_right_pid: resq 1
pipe_right_ptr: resq 1

section .note.GNU-stack noalloc noexec nowrite progbits
