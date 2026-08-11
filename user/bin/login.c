#include <crypt.h>
#include <pwdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Burned when the account does not exist, so a bad username costs the same time
   as a bad password and the prompt does not leak which accounts are real. */
#define DUMMY_HASH "$A1$xxxxxxxx$" \
    "0000000000000000000000000000000000000000000000000000000000000000"

static long read_line(char *buf, long maxlen)
{
    long n = read(0, buf, (size_t)maxlen - 1);
    if (n < 0) n = 0;
    if (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = '\0';
    return n;
}

int main(void)
{
    static char username[PW_NAME_MAX];
    static char password[128];
    static char stored[CRYPT_MAX];

    for (;;) {
        printf("\nAetherOS login: ");
        if (read_line(username, sizeof(username)) == 0) continue;

        printf("Password: ");
        read_line(password, sizeof(password));

        pwent_t pw;
        int known = (pw_by_name(username, &pw) == 0) &&
                    (sh_get(username, stored, sizeof(stored)) == 0);
        int ok = crypt_verify(password, known ? stored : DUMMY_HASH) && known;

        /* Don't leave the cleartext sitting in the static buffer afterwards. */
        memset(password, 0, sizeof(password));

        if (!ok) {
            /* One message for every failure: unknown user, wrong password, and
               locked account are indistinguishable from out here. */
            puts("Login incorrect.");
            continue;
        }

        printf("Welcome to AetherOS, %s!\n", username);

        if (pw.home[0]) chdir(pw.home);

        char *argv[] = { "shell", NULL };
        pid_t pid = spawn_as("/bin/shell", argv, pw.uid, pw.gid);
        if (pid < 0) {
            puts("login: could not start shell");
            continue;
        }
        waitpid(pid, NULL, 0);
    }
    return 0;
}
