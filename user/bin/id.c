#include <pwdb.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();

    /* Groups have no database of their own yet: useradd gives each account a
       gid equal to its uid, so the passwd entry doubles as the group name. */
    pwent_t upw, gpw;
    int have_uname = (pw_by_uid(uid, &upw) == 0);
    int have_gname = (pw_by_uid((uid_t)gid, &gpw) == 0);

    if (have_uname) printf("uid=%u(%s) ", uid, upw.name);
    else            printf("uid=%u ", uid);

    if (have_gname) printf("gid=%u(%s)\n", gid, gpw.name);
    else            printf("gid=%u\n", gid);

    return 0;
}
