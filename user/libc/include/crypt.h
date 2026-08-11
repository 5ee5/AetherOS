#ifndef _CRYPT_H
#define _CRYPT_H

/* Password hashing for AetherOS.
 *
 * Stored form:  $A1$<salt>$<64 lowercase hex digits>
 *
 *   A1    KDF identifier (AetherOS v1). Not compatible with crypt(3); this is
 *         our own construction and only ever compared against itself.
 *   salt  CRYPT_SALT_LEN characters from [A-Za-z0-9./]
 *   hex   SHA-256 of the iterated construction described in crypt.c
 *
 * A stored string that is empty, "!", or not in the form above is a LOCKED
 * account: no password can ever satisfy it. That is deliberate — an empty field
 * used to mean "no password required", which let every freshly created account
 * log in and (via sudo) become root. */

#define CRYPT_SALT_LEN   8
#define CRYPT_MAX      128   /* buffer size for a stored hash string */
#define CRYPT_LOCKED   "!"

/* Fill out[] with a fresh salt (CRYPT_SALT_LEN chars + NUL). */
void crypt_gensalt(char *out);

/* Write the stored form of `password` into out[CRYPT_MAX].
   `salt` may be NULL to generate one. Returns 0 on success, -1 on bad input. */
int crypt_make(const char *password, const char *salt, char *out);

/* 1 if `password` satisfies `stored`, else 0. Locked/malformed stored strings
   always return 0. The digest comparison is constant time. */
int crypt_verify(const char *password, const char *stored);

/* 1 if `stored` can never be satisfied (empty, "!", or malformed). */
int crypt_is_locked(const char *stored);

#endif
