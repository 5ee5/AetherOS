#ifndef KERNEL_SECURITY_CRED_H
#define KERNEL_SECURITY_CRED_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
} cred_t;

/* Initialise credentials for a new process (default: uid=1000, gid=1000). */
void cred_init(cred_t *c, uint32_t uid, uint32_t gid);

/* Return true if cred `c` may access a file with owner `fuid`/`fgid`
   and permissions `mode` (low 9 bits: rwxrwxrwx).
   `access` is a bitmask: 4=read, 2=write, 1=exec. */
bool cred_check(const cred_t *c, uint32_t fuid, uint32_t fgid,
                uint16_t mode, uint8_t access);

#endif
