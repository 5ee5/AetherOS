#include <pwdb.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    uid_t uid = getuid();

    pwent_t pw;
    if (pw_by_uid(uid, &pw) == 0) {
        puts(pw.name);
    } else {
        /* No matching entry (or /etc/passwd unreadable): the number is still
           true, and is more useful than nothing. */
        printf("%u\n", uid);
    }
    return 0;
}
