#include <crypt.h>
#include <pwdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (getuid() != 0) { puts("useradd: permission denied (must be root)"); return 1; }
    if (argc < 2) { puts("useradd: usage: useradd <username>"); return 1; }

    const char *username = argv[1];

    /* Basic username validation: non-empty, no colon or slash. */
    if (!username[0] || strchr(username, ':') || strchr(username, '/')) {
        puts("useradd: invalid username");
        return 1;
    }
    if (strlen(username) >= PW_NAME_MAX) {
        puts("useradd: username too long");
        return 1;
    }

    pwent_t existing;
    if (pw_by_name(username, &existing) == 0) {
        printf("useradd: user '%s' already exists\n", username);
        return 1;
    }

    uid_t max_uid = pw_max_uid();
    uid_t new_uid = (max_uid < 1000U) ? 1000U : max_uid + 1U;

    pwent_t e;
    strncpy(e.name, username, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.uid = new_uid;
    e.gid = new_uid;
    snprintf(e.home, sizeof(e.home), "/home/%s", username);

    mkdir("/home", 0);
    if (mkdir(e.home, 0) < 0) {
        printf("useradd: warning: could not create %s\n", e.home);
    } else {
        chown(e.home, new_uid, new_uid);
    }

    if (pw_add(&e) != 0) {
        puts("useradd: cannot write /etc/passwd");
        return 1;
    }

    /* Create the account LOCKED.  It used to be created with an empty password,
       which meant anyone could log in as it without one — and then use sudo to
       become root.  A locked account cannot be logged into at all until root
       gives it a password with `passwd <user>`. */
    if (sh_set(username, CRYPT_LOCKED) != 0) {
        puts("useradd: cannot write /etc/shadow (account left locked)");
        return 1;
    }

    printf("User '%s' created (uid=%u), login disabled.\n", username, new_uid);
    printf("Run 'passwd %s' to set a password and enable it.\n", username);
    return 0;
}
