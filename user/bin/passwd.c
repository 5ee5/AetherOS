#include <crypt.h>
#include <pwdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* /bin/passwd is installed setuid root, because /etc/shadow is 0600 and an
   unprivileged user must still be able to change their own password.  Everything
   below therefore runs with euid 0: check the *real* uid for every authorization
   decision, never the effective one. */

static long read_line(char *buf, long maxlen)
{
    long n = read(0, buf, (size_t)maxlen - 1);
    if (n < 0) n = 0;
    if (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = '\0';
    return n;
}

int main(int argc, char **argv)
{
    uid_t ruid = getuid();
    char target[PW_NAME_MAX];

    if (argc >= 2) {
        if (ruid != 0) {
            puts("passwd: only root can change another user's password");
            return 1;
        }
        strncpy(target, argv[1], sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    } else {
        pwent_t me;
        if (pw_by_uid(ruid, &me) != 0) {
            puts("passwd: current user not found in /etc/passwd");
            return 1;
        }
        strncpy(target, me.name, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    }

    pwent_t pw;
    if (pw_by_name(target, &pw) != 0) {
        printf("passwd: user '%s' not found\n", target);
        return 1;
    }

    static char cur[128], np1[128], np2[128];
    static char stored[CRYPT_MAX];
    int rc = 1;

    /* Non-root must prove they already know the current password.  A locked
       account cannot be unlocked this way — only root can set a password on it. */
    if (ruid != 0) {
        if (sh_get(target, stored, sizeof(stored)) != 0 ||
            crypt_is_locked(stored)) {
            puts("passwd: account is locked; ask root to set a password");
            return 1;
        }
        printf("Current password: ");
        read_line(cur, sizeof(cur));
        int ok = crypt_verify(cur, stored);
        memset(cur, 0, sizeof(cur));
        if (!ok) {
            puts("passwd: incorrect current password");
            return 1;
        }
    }

    printf("New password: ");
    read_line(np1, sizeof(np1));
    printf("Retype new password: ");
    read_line(np2, sizeof(np2));

    if (strcmp(np1, np2) != 0) {
        puts("passwd: passwords do not match");
        goto done;
    }
    if (np1[0] == '\0') {
        /* An empty hash is a locked account, so an empty password would lock the
           user out rather than let them log in freely.  Refuse it explicitly. */
        puts("passwd: empty passwords are not allowed");
        goto done;
    }

    char hash[CRYPT_MAX];
    if (crypt_make(np1, NULL, hash) != 0) {
        puts("passwd: could not hash password");
        goto done;
    }
    if (sh_set(target, hash) != 0) {
        puts("passwd: cannot write /etc/shadow");
        goto done;
    }

    printf("Password updated for %s.\n", target);
    rc = 0;

done:
    memset(np1, 0, sizeof(np1));
    memset(np2, 0, sizeof(np2));
    return rc;
}
