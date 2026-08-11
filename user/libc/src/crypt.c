/* Password hashing. Self-contained (only sha256.h + stdint.h) so the host tool
   tools/mkpasswd.c can build the identical KDF and the image build can install a
   hash the target agrees with. */

#include "crypt.h"
#include "sha256.h"

#include <stdint.h>

/* Iteration count. Each round is one SHA-256 compression over ~100 bytes, so
   5000 rounds is well under a millisecond of CPU — enough to make a stolen
   /etc/shadow meaningfully more expensive to attack than a bare digest, without
   a noticeable pause at the login prompt. */
#define ROUNDS 5000

#define SALT_ALPHABET "abcdefghijklmnopqrstuvwxyz" \
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
                      "0123456789./"

static uint32_t c_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int c_prefix(const char *s, const char *pfx)
{
    while (*pfx) { if (*s != *pfx) return 0; s++; pfx++; }
    return 1;
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Salts need to be *unique* far more than unpredictable: their job is to stop
   one precomputed table from covering every account. There is no entropy pool
   to draw on here, so mix the cycle counter (differs per call), a stack address,
   and a call counter, then run the mixture through SHA-256. */
void crypt_gensalt(char *out)
{
    static uint64_t counter;
    struct { uint64_t tsc1, tsc2, addr, ctr; } seed;

    seed.tsc1 = rdtsc();
    seed.addr = (uint64_t)(uintptr_t)&seed;
    seed.ctr  = ++counter;
    seed.tsc2 = rdtsc();

    uint8_t d[SHA256_DIGEST_LEN];
    sha256(&seed, (uint32_t)sizeof(seed), d);

    const char *alpha = SALT_ALPHABET;
    uint32_t n = c_strlen(alpha);
    for (int i = 0; i < CRYPT_SALT_LEN; ++i)
        out[i] = alpha[d[i] % n];
    out[CRYPT_SALT_LEN] = '\0';
}

/* h = SHA256(salt || pw); then ROUNDS-1 times h = SHA256(h || salt || pw).
   Feeding the salt and password back in every round keeps each iteration
   dependent on both, so the work cannot be precomputed for a salt alone. */
static void kdf(const char *password, const char *salt,
                uint8_t out[SHA256_DIGEST_LEN])
{
    uint32_t plen = c_strlen(password);
    uint32_t slen = c_strlen(salt);

    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, salt, slen);
    sha256_update(&c, password, plen);
    sha256_final(&c, out);

    for (int i = 1; i < ROUNDS; ++i) {
        sha256_init(&c);
        sha256_update(&c, out, SHA256_DIGEST_LEN);
        sha256_update(&c, salt, slen);
        sha256_update(&c, password, plen);
        sha256_final(&c, out);
    }
}

int crypt_make(const char *password, const char *salt, char *out)
{
    char gen[CRYPT_SALT_LEN + 1];

    if (!password || !out) return -1;
    if (!salt) { crypt_gensalt(gen); salt = gen; }

    uint32_t slen = c_strlen(salt);
    if (slen == 0 || slen > 16U) return -1;
    for (uint32_t i = 0; i < slen; ++i)
        if (salt[i] == '$' || salt[i] == ':' || salt[i] == '\n') return -1;

    uint8_t d[SHA256_DIGEST_LEN];
    kdf(password, salt, d);

    static const char hex[] = "0123456789abcdef";
    uint32_t o = 0;
    out[o++] = '$'; out[o++] = 'A'; out[o++] = '1'; out[o++] = '$';
    for (uint32_t i = 0; i < slen; ++i) out[o++] = salt[i];
    out[o++] = '$';
    for (uint32_t i = 0; i < SHA256_DIGEST_LEN; ++i) {
        out[o++] = hex[(d[i] >> 4) & 0xfU];
        out[o++] = hex[d[i] & 0xfU];
    }
    out[o] = '\0';
    return 0;
}

int crypt_is_locked(const char *stored)
{
    if (!stored || stored[0] == '\0') return 1;
    if (!c_prefix(stored, "$A1$")) return 1;

    /* Must have a second '$' with a non-empty salt, then exactly 64 hex. */
    const char *salt = stored + 4;
    const char *dollar = salt;
    while (*dollar && *dollar != '$') dollar++;
    if (*dollar != '$' || dollar == salt) return 1;

    const char *hexpart = dollar + 1;
    uint32_t n = 0;
    for (; hexpart[n]; ++n) {
        char ch = hexpart[n];
        int ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!ok) return 1;
    }
    return n == SHA256_DIGEST_LEN * 2U ? 0 : 1;
}

int crypt_verify(const char *password, const char *stored)
{
    if (!password || crypt_is_locked(stored)) return 0;

    /* Re-derive using the salt embedded in `stored`. */
    char salt[17];
    const char *s = stored + 4;
    uint32_t i = 0;
    while (s[i] && s[i] != '$' && i < sizeof(salt) - 1U) { salt[i] = s[i]; i++; }
    if (s[i] != '$') return 0;
    salt[i] = '\0';

    char mine[CRYPT_MAX];
    if (crypt_make(password, salt, mine) != 0) return 0;

    /* Constant-time compare over the whole stored string: OR the differences
       instead of returning early, so a wrong guess costs the same time however
       many leading digits it happened to get right. */
    uint32_t a = c_strlen(mine), b = c_strlen(stored);
    volatile uint8_t diff = (uint8_t)((a ^ b) != 0);
    uint32_t n = a < b ? a : b;
    for (uint32_t k = 0; k < n; ++k)
        diff |= (uint8_t)(mine[k] ^ stored[k]);
    return diff == 0;
}
