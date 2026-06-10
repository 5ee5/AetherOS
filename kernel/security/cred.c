#include "security/cred.h"
#include <stdbool.h>

void cred_init(cred_t *c, uint32_t uid, uint32_t gid)
{
    c->uid  = uid;
    c->gid  = gid;
    c->euid = uid;
    c->egid = gid;
    c->suid = uid;
    c->sgid = gid;
}

bool cred_setuid(cred_t *c, uint32_t new_uid)
{
    if (c->euid == 0) {
        c->uid = c->euid = c->suid = new_uid;
        return true;
    }
    if (new_uid == c->uid || new_uid == c->suid) {
        c->euid = new_uid;  /* effective-only change */
        return true;
    }
    return false;
}

bool cred_seteuid(cred_t *c, uint32_t new_uid)
{
    if (c->euid == 0 || new_uid == c->uid ||
        new_uid == c->euid || new_uid == c->suid) {
        c->euid = new_uid;
        return true;
    }
    return false;
}

bool cred_setgid(cred_t *c, uint32_t new_gid)
{
    if (c->euid == 0) {
        c->gid = c->egid = c->sgid = new_gid;
        return true;
    }
    if (new_gid == c->gid || new_gid == c->sgid) {
        c->egid = new_gid;
        return true;
    }
    return false;
}

bool cred_setegid(cred_t *c, uint32_t new_gid)
{
    if (c->euid == 0 || new_gid == c->gid ||
        new_gid == c->egid || new_gid == c->sgid) {
        c->egid = new_gid;
        return true;
    }
    return false;
}

bool cred_check(const cred_t *c, uint32_t fuid, uint32_t fgid,
                uint16_t mode, uint8_t access)
{
    /* root can do anything. */
    if (c->euid == 0) return true;

    uint8_t bits;
    if (c->euid == fuid)      bits = (uint8_t)((mode >> 6) & 7U); /* owner */
    else if (c->egid == fgid) bits = (uint8_t)((mode >> 3) & 7U); /* group */
    else                      bits = (uint8_t)(mode & 7U);         /* other */

    return (bits & access) == access;
}
