#ifndef Q2_HOOK_ORACLE_SHA256_H
#define Q2_HOOK_ORACLE_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
} hook_sha256_ctx_t;

void hook_sha256_init(hook_sha256_ctx_t *ctx);
void hook_sha256_update(hook_sha256_ctx_t *ctx, const void *data, size_t len);
void hook_sha256_final(hook_sha256_ctx_t *ctx, uint8_t hash[32]);
void hook_sha256_hex(const uint8_t hash[32], char out[65]);

#endif
