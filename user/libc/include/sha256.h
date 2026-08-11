#ifndef _SHA256_H
#define _SHA256_H

#include <stdint.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[SHA256_BLOCK_LEN];
    uint32_t buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, uint32_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience wrapper. */
void sha256(const void *data, uint32_t len, uint8_t out[SHA256_DIGEST_LEN]);

#endif
