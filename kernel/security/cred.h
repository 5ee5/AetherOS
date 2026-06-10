#ifndef KERNEL_SECURITY_CRED_H
#define KERNEL_SECURITY_CRED_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t uid;   /* real uid */
    uint32_t gid;   /* real gid */
    uint32_t euid;  /* effective uid */
    uint32_t egid;  /* effective gid */
    uint32_t suid;  /* saved set-uid — enables drop/restore of privilege */
    uint32_t sgid;  /* saved set-gid */
} cred_t;

/* Initialise credentials for a new process; real=effective=saved. */
void cred_init(cred_t *c, uint32_t uid, uint32_t gid);

/* Return true if cred `c` may access a file with owner `fuid`/`fgid`
   and permissions `mode` (low 9 bits: rwxrwxrwx).
   `access` is a bitmask: 4=read, 2=write, 1=exec. */
bool cred_check(const cred_t *c, uint32_t fuid, uint32_t fgid,
                uint16_t mode, uint8_t access);

/* POSIX-style credential transitions. Each returns true if permitted (and
   mutates `c`), false (EPERM) otherwise.
   - cred_setuid: if euid==0, sets real=eff=saved; otherwise permitted only if
     the target is the real or saved uid, and changes the effective uid only.
   - cred_seteuid: changes the effective uid only; permitted if euid==0 or the
     target is the real, effective, or saved uid. This is the drop/restore
     primitive (a setuid-root program can seteuid(user) then seteuid(0)). */
bool cred_setuid(cred_t *c, uint32_t new_uid);
bool cred_seteuid(cred_t *c, uint32_t new_uid);
bool cred_setgid(cred_t *c, uint32_t new_gid);
bool cred_setegid(cred_t *c, uint32_t new_gid);

#endif
