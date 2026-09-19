#ifndef HG_SHA256_H
#define HG_SHA256_H
typedef struct { unsigned int h[8]; unsigned char buf[64]; unsigned int n; unsigned long long len; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, unsigned int len);
void sha256_final(sha256_ctx *c, unsigned char out[32]);
#endif
