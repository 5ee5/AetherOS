#include <crypt.h>
#include <pwdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* /bin/sudo is setuid root, so euid is 0 the moment it starts.  Every decision
   below must therefore be made on the REAL uid — getuid(), never geteuid().
 *
 * Two independent gates, both of which must pass for a non-root caller:
 *
 *   1. Authorization — the caller's name appears in /etc/sudoers.  Missing or
 *      unreadable sudoers means nobody but root may use sudo (fail closed).
 *   2. Authentication — the caller retypes their own password, verified against
 *      /etc/shadow.  A locked account (no usable hash) can never pass.
 *
 * Gate 1 did not exist before: sudo authenticated the caller and then handed out
 * root to anyone who could type their own password — and skipped even that when
 * the password field was empty, which is how useradd left every new account. */

#define SUDOERS_PATH "/etc/sudoers"
#define SUDOERS_MAX  2048

static char s_sudoers[SUDOERS_MAX];

/* Whitespace trim in place; returns the start of the trimmed string. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    long n = (long)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

/* One account name per line; '#' starts a comment.  Returns 1 if `name` is
   listed. */
static int sudoers_allows(const char *name)
{
    int fd = open(SUDOERS_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;
    long n = read(fd, s_sudoers, SUDOERS_MAX - 1);
    close(fd);
    if (n <= 0) return 0;
    /* A file that fills the buffer would be parsed only as far as the cut, so an
       account listed past it is denied with no explanation.  Refusing outright is
       the same safe direction, but say so rather than failing silently. */
    if (n >= SUDOERS_MAX - 1) {
        puts("sudo: /etc/sudoers is too large to parse");
        return 0;
    }
    s_sudoers[n] = '\0';

    char *cur = s_sudoers;
    while (*cur) {
        char *line = cur;
        char *nl = strchr(cur, '\n');
        if (nl) { *nl = '\0'; cur = nl + 1; } else { cur += strlen(cur); }

        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        line = trim(line);
        if (line[0] == '\0') continue;
        if (strcmp(line, name) == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: sudo <command> [args...]");
        return 1;
    }

    uid_t ruid = getuid();

    /* Resolve a bare command name against /bin, the way the shell does — sudo
       used to pass argv[1] straight to spawn_as, so `sudo whoami` always failed
       and only absolute paths worked. */
    static char path[256];
    if (strchr(argv[1], '/') != NULL) {
        strncpy(path, argv[1], sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        snprintf(path, sizeof(path), "/bin/%s", argv[1]);
    }

    if (ruid != 0) {
        pwent_t me;
        if (pw_by_uid(ruid, &me) != 0) {
            puts("sudo: your account is not in /etc/passwd");
            return 1;
        }

        if (!sudoers_allows(me.name)) {
            printf("sudo: %s is not in the sudoers file\n", me.name);
            return 1;
        }

        static char stored[CRYPT_MAX];
        if (sh_get(me.name, stored, sizeof(stored)) != 0 ||
            crypt_is_locked(stored)) {
            puts("sudo: your account has no usable password");
            return 1;
        }

        printf("[sudo] password for %s: ", me.name);
        static char entered[128];
        long n = read(0, entered, sizeof(entered) - 1);
        if (n < 0) n = 0;
        if (n > 0 && entered[n - 1] == '\n') n--;
        entered[n] = '\0';

        int ok = crypt_verify(entered, stored);
        memset(entered, 0, sizeof(entered));

        if (!ok) {
            puts("sudo: authentication failure");
            return 1;
        }
    }

    pid_t pid = spawn_as(path, &argv[1], 0, 0);
    if (pid < 0) {
        printf("sudo: cannot execute %s\n", path);
        return 1;
    }
    waitpid(pid, NULL, 0);
    return 0;
}
