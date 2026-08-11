#ifndef _PWDB_H
#define _PWDB_H

#include <sys/types.h>

/* Shared reader/writer for the account database, so login, passwd, sudo,
   useradd and userdel stop each carrying their own copy of the parser.
 *
 *   /etc/passwd   name:x:uid:gid:home      mode 0644 (no secrets, so every
 *                                          program can resolve uid -> name)
 *   /etc/shadow   name:hash                mode 0600 (root only)
 *
 * The password field in /etc/passwd is always "x": the hash lives in
 * /etc/shadow. See crypt.h for the hash format and for what counts as a locked
 * account. */

#define PW_NAME_MAX 64
#define PW_HOME_MAX 192

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"

typedef struct {
    char  name[PW_NAME_MAX];
    uid_t uid;
    gid_t gid;
    char  home[PW_HOME_MAX];
} pwent_t;

/* Lookups. Return 0 and fill *out, or -1 if there is no such account. */
int pw_by_name(const char *name, pwent_t *out);
int pw_by_uid(uid_t uid, pwent_t *out);

/* Highest uid present in /etc/passwd (0 if the file is empty/unreadable). */
uid_t pw_max_uid(void);

/* Append an entry. Returns 0, or -1 on error (including a duplicate name). */
int pw_add(const pwent_t *e);

/* Remove an account from both /etc/passwd and /etc/shadow. 0 on success,
   -1 if absent or unwritable. */
int pw_del(const char *name);

/* Read the stored hash for `name` into hash[hashsz]. Returns 0 on success, -1
   if the account has no shadow entry or the file cannot be read (callers should
   treat -1 the same as a locked account). */
int sh_get(const char *name, char *hash, int hashsz);

/* Create or replace the shadow entry for `name`. Returns 0 on success, -1 on
   error. Requires write access to /etc/shadow (root, or a setuid-root caller
   such as /bin/passwd). */
int sh_set(const char *name, const char *hash);

#endif
