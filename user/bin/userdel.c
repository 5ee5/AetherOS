#include <pwdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (getuid() != 0) { puts("userdel: permission denied (must be root)"); return 1; }
    if (argc < 2) { puts("userdel: usage: userdel <username>"); return 1; }

    const char *username = argv[1];

    if (strcmp(username, "root") == 0) {
        puts("userdel: cannot delete root");
        return 1;
    }

    /* Removes the entry from /etc/passwd and its hash from /etc/shadow, so a
       later account reusing the name cannot inherit the old password. */
    if (pw_del(username) != 0) {
        printf("userdel: user '%s' not found\n", username);
        return 1;
    }

    printf("User '%s' deleted.\n", username);
    return 0;
}
