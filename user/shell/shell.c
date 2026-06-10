/* AetherOS interactive shell — C port of the original shell.asm.
   Built against tinylibc, linked at 0x400000 like the other user programs.
   Features (kept identical to the asm version):
     - prompt = cwd + " # " (root) or " $ "
     - line editing: backspace, 20-entry history via Up/Down arrows
     - builtins: echo (with > redirection), cat, ls, cd, pwd, clear, uname,
       help, exit, history
     - external commands resolved to /bin/<name>, run via spawn + waitpid
     - pipelines up to 4 stages ( cmd | cmd | ... )
     - background jobs ( cmd & ) */

#include <unistd.h>
#include <string.h>

#define LINE_MAX      256
#define CWD_BUF_SZ    256
#define CAT_BUF_SZ    4096
#define LS_BUF_SZ     4096
#define MAX_PIPE_SEGS 4
#define MAX_ARGV      16
#define HIST_MAX      20

static char line_buf[LINE_MAX];

/* History ring buffer. */
static char history[HIST_MAX][LINE_MAX];
static int  hist_head;   /* next slot to write */
static int  hist_count;  /* entries stored (<= HIST_MAX) */
static int  hist_pos;    /* browse offset (0 = editing current line) */

/* ---- small output helpers ------------------------------------------------- */

static void puts_raw(const char *s)
{
    write(1, s, strlen(s));
}

static void write_n(char c, int n)
{
    char buf[64];
    while (n > 0) {
        int k = n > (int)sizeof(buf) ? (int)sizeof(buf) : n;
        memset(buf, c, (size_t)k);
        write(1, buf, (size_t)k);
        n -= k;
    }
}

/* Erase n characters already echoed on the current line. */
static void erase_input(int n)
{
    if (n <= 0) return;
    write_n('\b', n);
    write_n(' ', n);
    write_n('\b', n);
}

/* ---- history -------------------------------------------------------------- */

static void hist_add(const char *line)
{
    strncpy(history[hist_head], line, LINE_MAX - 1);
    history[hist_head][LINE_MAX - 1] = '\0';
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
    hist_pos = 0;
}

/* Replace the on-screen line with history[slot]; update *len. */
static void hist_show(int slot, int *len)
{
    erase_input(*len);
    strncpy(line_buf, history[slot], LINE_MAX - 1);
    line_buf[LINE_MAX - 1] = '\0';
    int n = (int)strlen(line_buf);
    *len = n;
    if (n > 0) write(1, line_buf, (size_t)n);
}

static void hist_up(int *len)
{
    if (hist_pos >= hist_count) return;        /* already at oldest */
    hist_pos++;
    int slot = ((hist_head - hist_pos) % HIST_MAX + HIST_MAX) % HIST_MAX;
    hist_show(slot, len);
}

static void hist_down(int *len)
{
    if (hist_pos == 0) return;                 /* not browsing */
    hist_pos--;
    if (hist_pos == 0) {                        /* back to empty line */
        erase_input(*len);
        *len = 0;
        line_buf[0] = '\0';
        return;
    }
    int slot = ((hist_head - hist_pos) % HIST_MAX + HIST_MAX) % HIST_MAX;
    hist_show(slot, len);
}

/* ---- readline ------------------------------------------------------------- */
/* Reads one line into line_buf (NUL-terminated, newline stripped).
   Returns the number of characters. */
static int readline(void)
{
    int len = 0;
    line_buf[0] = '\0';

    for (;;) {
        char c;
        if (read(0, &c, 1) <= 0) break;
        unsigned char u = (unsigned char)c;

        if (u == 0x08 || u == 0x7f) {           /* backspace / DEL */
            if (len > 0) {
                len--;
                line_buf[len] = '\0';
                write(1, "\b \b", 3);
            }
            continue;
        }
        if (u == 0x1b) {                         /* ESC — arrow keys */
            char c1, c2;
            if (read(0, &c1, 1) <= 0) break;
            if (c1 != '[') continue;
            if (read(0, &c2, 1) <= 0) break;
            if (c2 == 'A') hist_up(&len);
            else if (c2 == 'B') hist_down(&len);
            continue;
        }
        if (u == '\n' || u == '\r') {            /* end of line */
            write(1, "\n", 1);
            line_buf[len] = '\0';
            if (len > 0) hist_add(line_buf);
            hist_pos = 0;
            return len;
        }
        if (u < 32) continue;                    /* ignore other controls */
        if (len < LINE_MAX - 1) {
            line_buf[len++] = c;
            line_buf[len] = '\0';
            write(1, &c, 1);                     /* local echo */
        }
    }

    line_buf[len] = '\0';
    return len;
}

/* ---- command helpers ------------------------------------------------------ */

static char *skip_spaces(char *p)
{
    while (*p == ' ') p++;
    return p;
}

/* Resolve a bare command name to /bin/<name>; paths with '/' are used as-is. */
static char bin_path[LINE_MAX + 8];
static const char *resolve_path(const char *cmd)
{
    for (const char *s = cmd; *s; s++)
        if (*s == '/') return cmd;
    int i = 0;
    bin_path[i++] = '/'; bin_path[i++] = 'b'; bin_path[i++] = 'i';
    bin_path[i++] = 'n'; bin_path[i++] = '/';
    for (const char *s = cmd; *s && i < (int)sizeof(bin_path) - 1; s++)
        bin_path[i++] = *s;
    bin_path[i] = '\0';
    return bin_path;
}

/* Split "cmd arg arg ..." into argv[] (cmd + up to 8 args), mutating the
   string in place. Returns argc. */
static char *argv_buf[MAX_ARGV];
static int build_argv(char *cmd, char *args)
{
    int n = 0;
    argv_buf[n++] = cmd;
    char *a = skip_spaces(args);
    while (*a && n < 9) {                         /* cmd + 8 args max */
        argv_buf[n++] = a;
        while (*a && *a != ' ') a++;
        if (*a == ' ') { *a = '\0'; a = skip_spaces(a + 1); }
    }
    argv_buf[n] = (char *)0;
    return n;
}

/* Tokenize one pipeline segment and spawn it with the given stdin/stdout fds. */
static int spawn_command(char *seg, int in_fd, int out_fd)
{
    char *cmd = skip_spaces(seg);
    if (!*cmd) return -1;
    char *args = cmd;
    while (*args && *args != ' ') args++;
    if (*args == ' ') { *args = '\0'; args++; }
    build_argv(cmd, args);
    return spawn_io(resolve_path(cmd), argv_buf, in_fd, out_fd);
}

/* ---- builtins ------------------------------------------------------------- */

static const char *err_notfound = "command not found\n";
static const char *err_chdir    = "cd: no such directory\n";
static const char *err_redir    = "echo: cannot open output file\n";
static const char *err_pipe     = "pipe: failed\n";

static const char *uname_str = "AetherOS 2.0 x86_64 (hobby kernel)\n";

static const char *help_text =
    "Built-in commands:\n"
    "  echo [text]   - print text (supports > redirection)\n"
    "  cat <path>    - print file contents\n"
    "  ls [path]     - list directory\n"
    "  cd [path]     - change directory\n"
    "  pwd           - print working directory\n"
    "  clear         - clear screen\n"
    "  uname         - print OS info\n"
    "  help          - this help\n"
    "  exit          - quit shell\n"
    "Pipes:     cmd1 | cmd2 | cmd3  (up to 4 stages)\n"
    "Background: cmd &\n"
    "Ctrl+C:    kill foreground process\n";

/* echo, honoring a trailing "> file" redirection. `args` is the text. */
static void builtin_echo(char *args)
{
    char *gt = 0;
    for (char *s = args; *s; s++)
        if (*s == '>') { gt = s; break; }

    if (gt) {
        char *l = gt - 1;
        while (l >= args && *l == ' ') l--;
        *(l + 1) = '\0';                         /* trim text */
        char *path = skip_spaces(gt + 1);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { puts_raw(err_redir); return; }
        int n = (int)strlen(args);
        if (n > 0) write(fd, args, (size_t)n);
        write(fd, "\n", 1);
        close(fd);
    } else {
        puts_raw(args);
        write(1, "\n", 1);
    }
}

static void builtin_cat(char *args)
{
    if (!*args) return;
    int fd = open(args, O_RDONLY, 0);
    if (fd < 0) { puts_raw(err_notfound); return; }
    static char cat_buf[CAT_BUF_SZ];
    ssize_t n;
    while ((n = read(fd, cat_buf, CAT_BUF_SZ)) > 0)
        write(1, cat_buf, (size_t)n);
    close(fd);
}

static void builtin_ls(char *args)
{
    char cwd[CWD_BUF_SZ];
    const char *path = args;
    if (!*args) {
        if (!getcwd(cwd, sizeof(cwd))) { cwd[0] = '/'; cwd[1] = '\0'; }
        path = cwd;
    }
    static char ls_buf[LS_BUF_SZ];
    long n = listdir(path, ls_buf, LS_BUF_SZ);
    if (n > 0) write(1, ls_buf, (size_t)n);
}

static void builtin_cd(char *args)
{
    const char *path = *args ? args : "/";
    if (chdir(path) != 0) puts_raw(err_chdir);
}

static void builtin_pwd(void)
{
    char cwd[CWD_BUF_SZ];
    if (!getcwd(cwd, sizeof(cwd))) { cwd[0] = '/'; cwd[1] = '\0'; }
    puts_raw(cwd);
    write(1, "\n", 1);
}

static void builtin_history(void)
{
    for (int i = 0; i < hist_count; i++) {
        int slot = ((hist_head - hist_count + i) % HIST_MAX + HIST_MAX) % HIST_MAX;
        puts_raw(history[slot]);
        write(1, "\n", 1);
    }
}

/* ---- execute -------------------------------------------------------------- */

static void run_single(char *seg, int bg)
{
    /* Split into command word + argument string. */
    char *cmd = seg;
    char *args = seg;
    while (*args && *args != ' ') args++;
    if (*args == ' ') { *args = '\0'; args = skip_spaces(args + 1); }

    if      (!strcmp(cmd, "echo"))    builtin_echo(args);
    else if (!strcmp(cmd, "cat"))     builtin_cat(args);
    else if (!strcmp(cmd, "ls"))      builtin_ls(args);
    else if (!strcmp(cmd, "cd"))      builtin_cd(args);
    else if (!strcmp(cmd, "pwd"))     builtin_pwd();
    else if (!strcmp(cmd, "clear"))   write(1, "\x1b[2J\x1b[H", 7);
    else if (!strcmp(cmd, "uname"))   puts_raw(uname_str);
    else if (!strcmp(cmd, "help"))    puts_raw(help_text);
    else if (!strcmp(cmd, "exit"))    _exit(0);
    else if (!strcmp(cmd, "history")) builtin_history();
    else {
        /* External command. */
        build_argv(cmd, args);
        int pid = spawn(resolve_path(cmd), argv_buf);
        if (pid < 0) { puts_raw(err_notfound); return; }
        if (!bg) waitpid(pid, 0, 0);
    }
}

static void run_pipeline(char **segs, int nseg, int bg)
{
    int pipes[MAX_PIPE_SEGS - 1][2];
    int npipe = nseg - 1;

    for (int i = 0; i < npipe; i++) {
        if (pipe(pipes[i]) < 0) { puts_raw(err_pipe); return; }
    }

    int pids[MAX_PIPE_SEGS];
    for (int i = 0; i < nseg; i++) {
        int in_fd  = (i == 0)        ? -1 : pipes[i - 1][0];
        int out_fd = (i == nseg - 1) ? -1 : pipes[i][1];
        pids[i] = spawn_command(segs[i], in_fd, out_fd);
    }

    /* Parent closes all pipe fds so EOF propagates. */
    for (int i = 0; i < npipe; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (bg) return;
    for (int i = nseg - 1; i >= 0; i--)
        if (pids[i] >= 0) waitpid(pids[i], 0, 0);
}

static void execute(char *line)
{
    char *p = skip_spaces(line);
    if (!*p) return;

    /* Background: a trailing '&' (after optional spaces). */
    int bg = 0;
    char *end = p + strlen(p) - 1;
    while (end >= p && *end == ' ') end--;
    if (end >= p && *end == '&') {
        *end = '\0';
        bg = 1;
        end--;
        while (end >= p && *end == ' ') { *end = '\0'; end--; }
    }
    if (!*p) return;

    /* Split on '|' into up to MAX_PIPE_SEGS segments. */
    char *segs[MAX_PIPE_SEGS];
    int nseg = 0;
    segs[nseg++] = p;
    for (char *q = p; *q; q++) {
        if (*q != '|') continue;
        if (nseg >= MAX_PIPE_SEGS) continue;     /* ignore extra | */
        char *l = q - 1;                          /* trim spaces before | */
        while (l >= p && *l == ' ') l--;
        *(l + 1) = '\0';
        char *r = skip_spaces(q + 1);             /* segment after | */
        segs[nseg++] = r;
        q = r - 1;                                /* continue scan after | */
    }

    if (nseg == 1) run_single(segs[0], bg);
    else           run_pipeline(segs, nseg, bg);
}

/* ---- entry ---------------------------------------------------------------- */

int main(void)
{
    puts_raw("AetherOS shell \xe2\x80\x94 type 'help' for commands\n");

    for (;;) {
        char cwd[CWD_BUF_SZ];
        if (!getcwd(cwd, sizeof(cwd))) { cwd[0] = '/'; cwd[1] = '\0'; }
        puts_raw(cwd);
        puts_raw(getuid() == 0 ? " # " : " $ ");

        if (readline() == 0) continue;
        execute(line_buf);
    }
    return 0;
}
