/* Host tool: generate an AetherOS password hash for the image build.
 *
 * Built with the HOST compiler against the same user/libc/src/crypt.c and
 * sha256.c that the target uses, so a hash written into /etc/shadow at image
 * build time is one the on-target crypt_verify() will accept.  If those two ever
 * disagreed, root would silently be unable to log in.
 *
 *   mkpasswd <password> [salt]   print the stored hash form
 *   mkpasswd --selftest          verify SHA-256 against FIPS vectors and check
 *                                that the KDF round-trips
 */

#include <stdio.h>
#include <string.h>

#include "crypt.h"
#include "sha256.h"

static int hexeq(const unsigned char *digest, const char *expect)
{
    static const char hex[] = "0123456789abcdef";
    char got[SHA256_DIGEST_LEN * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LEN; ++i) {
        got[i * 2]     = hex[(digest[i] >> 4) & 0xf];
        got[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    got[SHA256_DIGEST_LEN * 2] = '\0';
    if (strcmp(got, expect) == 0) return 1;
    printf("  got      %s\n  expected %s\n", got, expect);
    return 0;
}

static int selftest(void)
{
    int fail = 0;
    unsigned char d[SHA256_DIGEST_LEN];

    /* FIPS 180-4 / NIST example vectors. */
    sha256("abc", 3, d);
    if (!hexeq(d, "ba7816bf8f01cfea414140de5dae2223"
                  "b00361a396177a9cb410ff61f20015ad")) {
        puts("FAIL: sha256(\"abc\")"); fail = 1;
    }

    sha256("", 0, d);
    if (!hexeq(d, "e3b0c44298fc1c149afbf4c8996fb924"
                  "27ae41e4649b934ca495991b7852b855")) {
        puts("FAIL: sha256(\"\")"); fail = 1;
    }

    /* 56 bytes: forces the extra padding block (length field would not fit). */
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    if (!hexeq(d, "248d6a61d20638b8e5c026930c3e6039"
                  "a33ce45964ff2167f6ecedd419db06c1")) {
        puts("FAIL: sha256(448-bit message)"); fail = 1;
    }

    /* One million 'a' — exercises long multi-block streaming. */
    {
        sha256_ctx c;
        sha256_init(&c);
        char chunk[1000];
        memset(chunk, 'a', sizeof(chunk));
        for (int i = 0; i < 1000; ++i) sha256_update(&c, chunk, sizeof(chunk));
        sha256_final(&c, d);
        if (!hexeq(d, "cdc76e5c9914fb9281a1c7e284d73e67"
                      "f1809a48a497200e046d39ccc7112cd0")) {
            puts("FAIL: sha256(1e6 * 'a')"); fail = 1;
        }
    }

    /* KDF: a fixed salt+password must be reproducible, must verify, and must
       reject a wrong password. */
    char h1[CRYPT_MAX], h2[CRYPT_MAX];
    if (crypt_make("hunter2", "abcdefgh", h1) != 0 ||
        crypt_make("hunter2", "abcdefgh", h2) != 0 || strcmp(h1, h2) != 0) {
        puts("FAIL: KDF not deterministic"); fail = 1;
    }
    if (!crypt_verify("hunter2", h1))  { puts("FAIL: correct password rejected"); fail = 1; }
    if (crypt_verify("hunter3", h1))   { puts("FAIL: wrong password accepted"); fail = 1; }
    if (crypt_verify("", h1))          { puts("FAIL: empty password accepted"); fail = 1; }

    /* Locked / malformed forms must never verify. */
    if (!crypt_is_locked(""))          { puts("FAIL: empty hash not locked"); fail = 1; }
    if (!crypt_is_locked(CRYPT_LOCKED)){ puts("FAIL: \"!\" not locked"); fail = 1; }
    if (!crypt_is_locked("root"))      { puts("FAIL: legacy plaintext not locked"); fail = 1; }
    if (!crypt_is_locked("$A1$salt$short")) { puts("FAIL: short digest not locked"); fail = 1; }
    if (crypt_verify("root", "root"))  { puts("FAIL: plaintext compare accepted"); fail = 1; }

    /* Distinct salts must give distinct hashes for the same password. */
    char s1[CRYPT_SALT_LEN + 1], s2[CRYPT_SALT_LEN + 1];
    crypt_gensalt(s1);
    crypt_gensalt(s2);
    if (strcmp(s1, s2) == 0) { puts("FAIL: two salts identical"); fail = 1; }
    crypt_make("same", s1, h1);
    crypt_make("same", s2, h2);
    if (strcmp(h1, h2) == 0) { puts("FAIL: salt not affecting hash"); fail = 1; }

    puts(fail ? "mkpasswd: SELFTEST FAILED" : "mkpasswd: selftest passed");
    return fail;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) return selftest();

    if (argc < 2) {
        fprintf(stderr, "usage: mkpasswd <password> [salt]\n"
                        "       mkpasswd --selftest\n");
        return 2;
    }

    char out[CRYPT_MAX];
    if (crypt_make(argv[1], argc >= 3 ? argv[2] : NULL, out) != 0) {
        fprintf(stderr, "mkpasswd: could not hash password\n");
        return 1;
    }
    puts(out);
    return 0;
}
