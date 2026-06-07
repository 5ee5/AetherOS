#include "security/cred.h"
#include <stdbool.h>

void cred_init(cred_t *c, uint32_t uid, uint32_t gid)
{
    c->uid  = uid;
    c->gid  = gid;
    c->euid = uid;
    c->egid = gid;
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
