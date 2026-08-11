/* FIPS 180-4 SHA-256.
 *
 * Deliberately self-contained — only <stdint.h> — because this file is built
 * twice: once for AetherOS userland against tinylibc, and once for the host
 * with the system compiler (tools/mkpasswd.c) so the image build can generate a
 * password hash the target will agree with. Pulling in either libc's string.h
 * would break one of those two builds. */

#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32U - n)); }

static void transform(sha256_ctx *c, const uint8_t block[64])
{
    uint32_t w[64];

    for (uint32_t i = 0; i < 16U; ++i) {
        w[i] = ((uint32_t)block[i * 4U] << 24) |
               ((uint32_t)block[i * 4U + 1U] << 16) |
               ((uint32_t)block[i * 4U + 2U] << 8) |
               ((uint32_t)block[i * 4U + 3U]);
    }
    for (uint32_t i = 16U; i < 64U; ++i) {
        uint32_t s0 = ror(w[i - 15U], 7) ^ ror(w[i - 15U], 18) ^ (w[i - 15U] >> 3);
        uint32_t s1 = ror(w[i - 2U], 17) ^ ror(w[i - 2U], 19) ^ (w[i - 2U] >> 10);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (uint32_t i = 0; i < 64U; ++i) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void sha256_init(sha256_ctx *c)
{
    c->state[0] = 0x6a09e667U; c->state[1] = 0xbb67ae85U;
    c->state[2] = 0x3c6ef372U; c->state[3] = 0xa54ff53aU;
    c->state[4] = 0x510e527fU; c->state[5] = 0x9b05688cU;
    c->state[6] = 0x1f83d9abU; c->state[7] = 0x5be0cd19U;
    c->bitlen = 0;
    c->buflen = 0;
}

void sha256_update(sha256_ctx *c, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; ++i) {
        c->buf[c->buflen++] = p[i];
        if (c->buflen == SHA256_BLOCK_LEN) {
            transform(c, c->buf);
            c->bitlen += 512U;
            c->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN])
{
    uint32_t i = c->buflen;

    /* Append 0x80, pad with zeros, leaving 8 bytes for the length. */
    c->buf[i++] = 0x80U;
    if (i > 56U) {
        while (i < SHA256_BLOCK_LEN) c->buf[i++] = 0;
        transform(c, c->buf);
        i = 0;
    }
    while (i < 56U) c->buf[i++] = 0;

    uint64_t bits = c->bitlen + (uint64_t)c->buflen * 8U;
    for (uint32_t b = 0; b < 8U; ++b)
        c->buf[56U + b] = (uint8_t)(bits >> (56U - 8U * b));
    transform(c, c->buf);

    for (uint32_t w = 0; w < 8U; ++w) {
        out[w * 4U]      = (uint8_t)(c->state[w] >> 24);
        out[w * 4U + 1U] = (uint8_t)(c->state[w] >> 16);
        out[w * 4U + 2U] = (uint8_t)(c->state[w] >> 8);
        out[w * 4U + 3U] = (uint8_t)(c->state[w]);
    }
}

void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_LEN])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}
